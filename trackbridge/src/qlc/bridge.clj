(ns qlc.bridge
  "TRACK's beat-link helper (track-app branch, Tobias 11-04: \"byg de to vi lige har
  lagt i backloggen\" - beat-link som hjaelpeproces).

  QLC+ (TRACK.exe --track-app) starts this as a hidden Java process instead of
  Beat Link Trigger. It runs our qlc_shared_functions.clj UNCHANGED on beat-link -
  the same classes BLT runs it on: BLT 8.0.0's own jar from TRACK's runtime folder (r548)
  or an installed BLT's - and calls it the
  way BLT's one trigger did:
      Watch:            Master Player
      Beat:             (qlc-beat! beat-number)
      Tracked Update:   (qlc-tracked-update! device-number beat-number playing?)
  and it goes online the way BLT does (the same finders, the same RGB waveforms,
  no real player number, metadata passive when a player allows only three
  database clients).

  Only READS from the decks and the mixer: like BLT it announces itself as a
  virtual player to get metadata, and it sends nothing that steers a deck - no
  status packets, no sync, no master, no loads.

  One beat-link program at a time: it takes BLT's own PC lock
  (%ProgramData%\\qlc-rig\\blt-instance.lock, the startup guard's). While BLT
  runs, this exits with code 3 and QLC+ tries again later; while this runs, a
  guarded BLT says it is already running."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [taoensso.timbre :as timbre]
            [taoensso.timbre.appenders.community.rotor :as rotor])
  (:import [java.nio.channels FileChannel]
           [java.nio.file OpenOption StandardOpenOption]
           [org.deepsymmetry.beatlink Beat BeatFinder BeatListener CdjStatus DeviceAnnouncementListener
            DeviceFinder DeviceUpdateListener LifecycleListener VirtualCdj]
           [org.deepsymmetry.beatlink.data AnalysisTagFinder BeatGridFinder CrateDigger MetadataFinder
            TimeFinder WaveformFinder WaveformFinder$WaveformStyle]))

(def bridge-version "3 (track-app r561)")

;; ---------------------------------------------------------------- places

(defn- windows? [] (str/starts-with? (str/lower-case (System/getProperty "os.name" "")) "windows"))

(defn- log-dir
  "TRACK's own logs folder (-Dqlc.bridge.logdir, R548_PLACES), else beside QLC+'s and the
  guard's startup.log on the lys-PC."
  ^java.io.File []
  (let [given (System/getProperty "qlc.bridge.logdir")]
    (cond
      (not (str/blank? given)) (io/file given)
      (and (windows?) (System/getenv "LOCALAPPDATA")) (io/file (System/getenv "LOCALAPPDATA") "qlc-rig-update")
      :else (io/file (System/getProperty "user.home") ".qlc-rig-update"))))

(defn- lock-file
  "The BLT startup guard's lock: one beat-link program on the PC at a time."
  ^java.io.File []
  (if-let [shared (and (windows?) (System/getenv "ProgramData"))]
    (io/file shared "qlc-rig" "blt-instance.lock")
    (io/file (System/getProperty "user.home") ".qlc-rig" "blt-instance.lock")))

(defn- shared-file
  "-Dqlc.bridge.clj=<file>, or nil = the copy in our jar. R548 (Tobias 11-04: \"den skal kunne
  deployes paa en ren pc\"): TRACK runs the functions it was built with, not BLT's copy in
  Downloads\\qlc-update - TRACK updates them itself."
  []
  (let [given (System/getProperty "qlc.bridge.clj")]
    (when-not (str/blank? given)
      (io/file given))))

;; ---------------------------------------------------------------- the log

(defn- start-log! []
  (let [dir (log-dir)]
    (.mkdirs dir)
    (Thread/setDefaultUncaughtExceptionHandler
     (reify Thread$UncaughtExceptionHandler
       (uncaughtException [_ thread e]
         (timbre/error e "Uncaught exception on" (.getName thread)))))
    (timbre/set-config!
     {:min-level      :info
      :ns-filter      #{"*"}
      :middleware     []
      :timestamp-opts {:pattern  "yyyy-MM-dd HH:mm:ss.SSS"
                       :locale   :jvm-default
                       :timezone (java.util.TimeZone/getDefault)}
      :output-fn      timbre/default-output-fn
      :appenders      {:rotor (rotor/rotor-appender {:path     (str (io/file dir "bridge.log"))
                                                     :max-size 2000000
                                                     :backlog  5})}})
    (when (System/getProperty "qlc.bridge.console")
      (timbre/merge-config! {:appenders {:println (timbre/println-appender {:stream :std-err})}}))
    (timbre/info "TRACK beat-link helper" bridge-version "starting; Java" (System/getProperty "java.version")
                 "; beat-link" (or (try (with-open [r (io/reader (io/resource "META-INF/maven/org.deepsymmetry/beat-link/pom.properties"))]
                                          (some #(when (str/starts-with? % "version=") (subs % 8)) (doall (line-seq r))))
                                        (catch Throwable _ nil))
                                   "?"))))

;; ---------------------------------------------------------------- one beat-link program at a time

(defonce ^:private held-lock (atom nil))   ; kept for the JVM's whole life, never released by hand

(defn- take-lock!
  "True when we hold the PC's beat-link lock. Never deletes the file."
  []
  (try
    (let [f (lock-file)]
      (.mkdirs (.getParentFile f))
      (let [ch (FileChannel/open (.toPath f) (into-array OpenOption [StandardOpenOption/CREATE StandardOpenOption/WRITE]))
            lk (.tryLock ch)]
        (if lk
          (do (reset! held-lock [ch lk]) true)
          (do (.close ch) false))))
    (catch java.nio.channels.OverlappingFileLockException _ false)
    (catch Throwable e
      (timbre/error e "Could not take the beat-link lock")
      false)))

;; ---------------------------------------------------------------- the shared functions

(defonce ^:private shared-ns (atom nil))

(defn- load-shared!
  "Our qlc_shared_functions.clj, in a namespace of its own with the two aliases it
  uses (BLT's expression namespace has them: timbre and json)."
  []
  (let [target (create-ns 'qlc.shared)
        f      (shared-file)]
    (binding [*ns* target]
      (clojure.core/refer 'clojure.core)
      (require '[taoensso.timbre :as timbre] '[cheshire.core :as json] 'clojure.string)
      (if f
        (do (timbre/info "Loading the shared functions from" (str f))
            (load-file (str f)))
        (do (timbre/info "Loading the shared functions from the copy in trackbridge.jar")
            (with-open [r (io/reader (io/resource "qlc_shared_functions.clj") :encoding "UTF-8")]
              (clojure.lang.Compiler/load r "qlc_shared_functions.clj" "qlc_shared_functions.clj")))))
    (reset! shared-ns target)
    (timbre/info "Shared functions loaded:"
                 (count (filter #(str/starts-with? (str %) "qlc-") (keys (ns-interns target)))) "qlc- names")))

(defn- shared
  "The shared function's var (so a reload is picked up), or nil."
  [sym]
  (some-> @shared-ns (ns-resolve sym)))

;; ---------------------------------------------------------------- BLT's trigger, without BLT

(defonce ^:private failures (atom {}))

(defn- run-expression
  "As BLT's run-trigger-function: an exception is logged, never thrown into beat-link
  (each kind logged in full only the first times, then every 500th)."
  [kind f & args]
  (try
    (when f (apply f args))
    (catch Throwable t
      (let [n (get (swap! failures update kind (fnil inc 0)) kind)]
        (when (or (<= n 3) (zero? (mod n 500)))
          (timbre/error t "Problem running the" kind "expression (" n "times so far)"))))))

(defn- current-beat
  "BLT's beat-number for a beat packet: the TimeFinder's position for that player, or nil."
  [^Beat beat]
  (let [tf (TimeFinder/getInstance)]
    (when (.isRunning tf)
      (when-let [pos (.getLatestPositionFor tf beat)]
        (.-beatNumber pos)))))

(defn- shared-value
  "The value of a shared atom (deref of its var's atom), or nil."
  [sym]
  (try (some-> (shared sym) deref deref) (catch Throwable _ nil)))

(defn- cdj-master
  "The tempo master when it is a player 1-4, else nil (none, or a mixer / BLT as master)."
  []
  (try
    (let [vc (VirtualCdj/getInstance)]
      (when (.isRunning vc)
        (when-let [m (.getTempoMaster vc)]
          (let [n (long (.getDeviceNumber m))]
            (when (<= 1 n 4) n)))))
    (catch Throwable _ nil)))

(defn- heard-decks
  "The shared functions' players the room hears (runde 203), as a sorted vector."
  []
  (try (vec (sort (or (some-> (shared 'qlc-heard-decks) (apply [])) #{}))) (catch Throwable _ [])))

(def ^:private status-listener
  (reify DeviceUpdateListener
    (received [_ status]
      (when (instance? CdjStatus status)
        (let [^CdjStatus s status
              dev (long (.getDeviceNumber s))]
          (cond
            ;; Watch: Master Player - a full status update from the tempo master
            (.isTempoMaster s)
            (run-expression :tracked (shared 'qlc-tracked-update!)
                            (.getDeviceNumber s) (.getBeatNumber s) (.isPlaying s))
            ;; R560_NO_MASTER (Tobias 11-05, the light stood still while a deck played): no player
            ;; is the tempo master (the DJ never pressed SYNC / MASTER) - BLT's trigger, and so far
            ;; this helper, then ran nothing at all. Now the deck the lights follow - or, with none,
            ;; the first one the room hears - stands in for the master, so runde 203's choice of
            ;; the heard deck still runs. One deck only, so the choice never flips between two.
            ;; R561: the shared functions stand in themselves now (qlc-standin-status!, also in the
            ;; rig's BLT) - this only runs with an older shared-functions file
            (and (<= 1 dev 4) (nil? (cdj-master)) (nil? (shared 'qlc-standin-status!)))
            (let [f (shared-value 'qlc-followed)
                  vc (VirtualCdj/getInstance)
                  f (when (and f (try (.getLatestStatusFor vc (int f)) (catch Throwable _ nil))) (long f))
                  stand-in (or f (first (heard-decks)))]
              (when (and stand-in (== dev stand-in))
                (run-expression :tracked (shared 'qlc-tracked-update!)
                                (.getDeviceNumber s) (.getBeatNumber s) (.isPlaying s))))))))))

(def ^:private beat-listener
  (reify BeatListener
    (newBeat [_ beat]
      (when (and (.isRunning (VirtualCdj/getInstance)) (.isTempoMaster ^Beat beat))
        (run-expression :beat (shared 'qlc-beat!) (or (current-beat beat) -1))))))

;; ---------------------------------------------------------------- R548_CDJS: what QLC+ shows

(defonce ^:private last-status (atom [nil 0]))
(defonce ^:private last-decks (atom [nil 0]))
(defonce ^:private last-next (atom [nil 0]))

(defn- report-decks!
  "R560_DECKS: \"DECKS m=<master> f=<followed> h=<heard> p=<playing>\" on stdout for QLC+ (trackbridge.cpp):
  which deck the lights follow and why - when it changes, and every 5 s. 0 / empty = none."
  []
  (try
    (let [vc (VirtualCdj/getInstance)
          playing (if (.isRunning vc)
                    (vec (for [p [1 2 3 4]
                               :let [st (.getLatestStatusFor vc (int p))]
                               :when (and (instance? CdjStatus st) (.isPlaying ^CdjStatus st))]
                           p))
                    [])
          line (str "DECKS m=" (or (cdj-master) 0)
                    " f=" (or (shared-value 'qlc-followed) 0)
                    " h=" (str/join "," (heard-decks))
                    " p=" (str/join "," playing))
          now (System/currentTimeMillis)
          [was at] @last-decks]
      (when (or (not= line was) (> (- now (long at)) 5000))
        (reset! last-decks [line now])
        (locking last-status
          (.println System/out line)
          (.flush System/out))))
    (catch Throwable _ nil)))

(defn- report-next!
  "R561_NEXT: \"NEXT d=<deck> s=<ready|analysing|failed> t=<title>\" on stdout for QLC+ (trackbridge.cpp): the
  deck the next track is probably on (the shared functions' guess), and whether the lights have its sections
  yet - READY once NEXT went to QLC+. When it changes, and every 5 s. d=0: none."
  []
  (try
    (let [f      (shared-value 'qlc-followed)
          guess  (when f (some-> (shared 'qlc-guess-next) (apply [f])))
          [p title] guess
          sent   (shared-value 'qlc-next-sent)
          failed (get (or (shared-value 'qlc-failed-at) {}) title)
          now    (System/currentTimeMillis)
          st     (cond (nil? title)                                "none"
                       (= title sent)                              "ready"
                       (and failed (< (- now (long failed)) 60000)) "failed"
                       :else                                       "analysing")
          line   (if (= st "none")
                   "NEXT d=0 s=none t="
                   (str "NEXT d=" p " s=" st " t=" (str/replace (str title) #"[\r\n]+" " ")))
          [was at] @last-next]
      (when (or (not= line was) (> (- now (long at)) 5000))
        (reset! last-next [line now])
        ;; the title as UTF-8 bytes whatever Windows' console code page is (trackbridge.cpp reads UTF-8)
        (let [b (.getBytes (str line "\n") "UTF-8")]
          (locking last-status
            (.write System/out b 0 (alength b))
            (.flush System/out)))))
    (catch Throwable _ nil)))

(defn- report-cdjs!
  "\"STATUS cdjs=N\" on stdout for QLC+ (trackbridge.cpp reads it): the players (numbers 1-6)
  on the DJ Link network. When it changes, and every 5 s."
  []
  (try
    (let [df  (DeviceFinder/getInstance)
          n   (if (.isRunning df)
                (count (filter #(<= 1 (.getDeviceNumber ^org.deepsymmetry.beatlink.DeviceAnnouncement %) 6)
                               (.getCurrentDevices df)))
                0)
          now (System/currentTimeMillis)
          [was at] @last-status]
      (when (or (not= n was) (> (- now (long at)) 5000))
        (reset! last-status [n now])
        (locking last-status
          (.println System/out (str "STATUS cdjs=" n))
          (.flush System/out))))
    (catch Throwable _ nil)))

;; ---------------------------------------------------------------- online, as BLT goes online

(defn- set-passive!
  "BLT's rebuild-all-device-status: no dbserver queries when a player allows only three clients."
  []
  (try
    (let [mf (MetadataFinder/getInstance)
          limited (.isAnyDeviceLimitedToThreeDatabaseClients (DeviceFinder/getInstance))]
      (when (not= limited (.isPassive mf))
        (timbre/info "Metadata finder passive:" limited)
        (.setPassive mf limited)))
    (catch Throwable t (timbre/warn t "Problem setting metadata passive mode"))))

(def ^:private device-listener
  (reify DeviceAnnouncementListener
    (deviceFound [_ a]
      (timbre/info "DJ Link device found:" (.getDeviceName a) "number" (.getDeviceNumber a) "at" (.getHostAddress (.getAddress a)))
      (set-passive!)
      (report-cdjs!))
    (deviceLost [_ a]
      (timbre/info "DJ Link device lost:" (.getDeviceName a) "number" (.getDeviceNumber a))
      (set-passive!)
      (report-cdjs!))))

(defn- start-finders!
  "BLT's start-other-finders (and its beat finder), without the album art and
  signatures nothing of ours reads."
  []
  (.setPreferredStyle (WaveformFinder/getInstance) WaveformFinder$WaveformStyle/RGB)
  (.start (BeatFinder/getInstance))
  (.start (MetadataFinder/getInstance))
  (.start (CrateDigger/getInstance))
  (.start (BeatGridFinder/getInstance))
  (.start (TimeFinder/getInstance))
  (.setFindDetails (WaveformFinder/getInstance) true)
  (.start (WaveformFinder/getInstance))
  (.start (AnalysisTagFinder/getInstance))
  (set-passive!)
  (timbre/info "Finders running: metadata, crate digger, beat grids, time, RGB waveforms, analysis tags"))

(defn- go-online!
  "One step of the online loop. BLT: start the DeviceFinder, wait until a DJ Link
  device is seen, start the VirtualCdj with no real player number, then the
  finders; lost the last device - offline, and look again."
  [state]
  (let [df   (DeviceFinder/getInstance)
        vcdj (VirtualCdj/getInstance)]
    (try
      (when-not (.isRunning df)
        (.start df))
      (let [devices (seq (.getCurrentDevices df))]
        (cond
          (and (not (.isRunning vcdj)) devices)
          (do (.setUseStandardPlayerNumber vcdj false)
              (if (.start vcdj)
                (do (timbre/info "Went online, using player number" (.getDeviceNumber vcdj))
                    (start-finders!)
                    :online)
                (do (timbre/warn "Unable to create the virtual CDJ - trying again") :looking)))

          (and (.isRunning vcdj) (not devices))
          (do (timbre/info "Lost the last DJ Link device - offline, looking again")
              (.stop vcdj)
              :looking)

          (.isRunning vcdj) :online

          :else (do (when (not= state :looking) (timbre/info "Looking for DJ Link devices ..."))
                    :looking)))
      (catch java.net.BindException e
        (when (not= state :port-busy)
          (timbre/error e "The DJ Link port is in use - is rekordbox or another DJ Link program running? Trying again"))
        :port-busy)
      (catch Throwable t
        (timbre/warn t "Problem going online - trying again")
        :error))))

(defn- watch-parent!
  "--parent-pipe: QLC+ holds our stdin; when it closes (QLC+ quit or crashed) we stop."
  []
  (doto (Thread. ^Runnable
         (fn []
           (try
             (let [in System/in]
               (while (not= -1 (.read in))))
             (catch Throwable _ nil))
           (timbre/info "QLC+ closed our pipe - stopping")
           (System/exit 0))
         "qlc-parent-pipe")
    (.setDaemon true)
    (.start)))

(defn- stop-beat-link!
  "The virtual CDJ leaves the network (its finders stop with it). The DeviceFinder is
  left to the JVM's exit: stopping it here raced beat-link's own lost-device delivery."
  []
  (try (when (.isRunning (VirtualCdj/getInstance)) (.stop (VirtualCdj/getInstance))) (catch Throwable _ nil)))

(defn -main [& args]
  (let [args (set args)]
    (start-log!)
    (when-not (take-lock!)
      (timbre/warn "Beat Link Trigger (or another TRACK helper) is running - this helper is not needed now")
      (shutdown-agents)
      (System/exit 3))
    (timbre/info "Holding the beat-link lock" (str (lock-file)))
    (when (contains? args "--parent-pipe")
      (watch-parent!))
    (.addShutdownHook (Runtime/getRuntime)
                      (Thread. ^Runnable (fn [] (timbre/info "Stopping beat-link") (stop-beat-link!))))
    ;; the listeners first, as BLT registers its trigger listeners before it goes online
    (.addDeviceAnnouncementListener (DeviceFinder/getInstance) device-listener)
    (.addUpdateListener (VirtualCdj/getInstance) status-listener)
    (.addBeatListener (BeatFinder/getInstance) beat-listener)
    (.addLifecycleListener (VirtualCdj/getInstance)
                           (reify LifecycleListener
                             (started [_ _] nil)
                             (stopped [_ _] (timbre/info "Virtual CDJ stopped"))))
    (try
      (load-shared!)
      (catch Throwable t
        (timbre/error t "The shared functions did not load - nothing to run")
        (System/exit 4)))
    (loop [state nil]
      (let [state* (go-online! state)]
        (report-cdjs!)
        (report-decks!)                  ; R560_DECKS
        (report-next!)                   ; R561_NEXT
        (Thread/sleep (if (= state* :online) 1000 500))
        (recur state*)))))

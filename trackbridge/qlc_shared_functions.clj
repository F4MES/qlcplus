;; ---------------------------------------------------------------------------
;;  QLC+ Track view link  --  Shared Functions
;;
;;  MAALING   pr. beat: level (lydstyrke), low (bas), kick (puls: hvor meget
;;            hoejere og roedere beatets attack er end resten af beatet).
;;  TAKTER    beatgrid'et kender downbeats. Frasefasen fittes blandt 8 takter.
;;  GRAENSER  overgange maales paa 2, 4 og 8 takters skala, saa baade trin og
;;            ramper fanges. Taersklen tilpasser sig laengden. Ingen sektion
;;            maa vaere over 32 takter - laengere deles ved staerkeste overgang.
;;            Hver graense flyttes til taktlinjen (+-2) hvor kicket skifter mest.
;;  NIVEAU    kick fra -> break. Kick tilbage efter break MED hoej energi -> drop.
;;            Ellers groove, medmindre energien loefter sig til fuld.
;;  BUILD     straekkes bagud fra drop'et saa laenge energien stiger (8-32 takter).
;; ---------------------------------------------------------------------------

(require '[cheshire.core :as json])
(import '[java.net Socket]
        '[java.io PrintWriter BufferedReader InputStreamReader])

;; Bump when the analysis changes: cached automatic results from an older
;; version are thrown away, manual corrections are kept.
(require 'clojure.string)      ; qlc-explain! bruger join; BLT har den allerede
                               ; indlaest, men det er ikke noget vi bestemmer

(def qlc-analysis-version 14)  ; 14: R520_TRACKANA_VER - det hoeje break paa 32+ takter taeller ogsaa som nummerets FOERSTE sektion (The Weeknd vs Otto Knows: 61 takter break, rettet 2 gange 30-31/10; 3 af 538 automatiske numre i cachen, 0 af 124 haandrettede og 0 af 90 rekordbox). 13: R505_SECT_LOUDGAP_VER - analyse 10 med et hoejt break paa 32+ takter faar den gamle analyse (17 af 523 numre). 12: 12: drop/normal-taersklerne fra loerdagens 26 rettelser - Chorus-gulv 0.75, kick-drop 0.70 (runde 354, Tobias: "Ja bare goer alt du skal"). 11: alle automatiske numre analyseres om (poster skadet foer 10c) + beat-grid'et i cachen (runde 295, Tobias: "ja det er fint bare cache forfra")
                               ; 10: sektionerne foelger kicket takt for takt (analyse 10, 2026-09-27)
                               ; 9: gentagelser loefter ikke til drop uden kick, sektioner holdes under 32 takter (runde 210)
                               ; 8: build/roll uden glatningens laek, drop kraever kick, drive+normal adskilt (runde 194)
                               ; 7: "drive" - hoej groove, og et drop uden opbygning
                               ; 6: break er relativt til nummerets eget spaend
;                                 5: Pioneers graensedetektor (US10629173B2) er med

;; ---------------------------------------------------------------- forbindelse

(def qlc-host "127.0.0.1")
(def qlc-port 9998)

(defonce qlc-conn (atom nil))
(def qlc-last-track (atom {}))
;; [player title] QLC+ has right now. qlc-last-track is per deck; when the
;; master went 1 -> 2 -> 1 with deck 1 unchanged, deck 1's title matched and
;; its track was never resent - QLC+ ran A's beats on B's flags (runde 187)
(def qlc-sent-track (atom nil))
(declare qlc-start-reader! qlc-title qlc-next-sent qlc-mixing qlc-hand-made?
         qlc-out qlc-mix-candidate)  ; defined below

;; runde 247: when the last connect failed, nobody tries again for two seconds.
;; The trigger thread's qlc-send-track! also connects, and while QLC+ was down
;; (a crash, a restart mid-show, BLT started first) every status update of a
;; loaded deck stalled BLT's status thread for the connect's whole timeout.
(defonce qlc-connect-failed-at (atom 0))

(defn qlc-disconnect!
  "Luk forbindelsen. Med et argument lukkes den KUN hvis det stadig er den
   samme forbindelse - ellers river en gammel skrivers fejl en helt frisk
   forbindelse ned, og saa gaar det i ring med genforbindelser."
  ([] (qlc-disconnect! nil))
  ([expected]
  (let [cur @qlc-conn]
    (when (and cur (or (nil? expected) (identical? (:socket expected) (:socket cur))))
      (let [{:keys [socket writer]} cur]
        (try (.close writer) (catch Exception _ nil))
        (try (.close socket) (catch Exception _ nil))
        (compare-and-set! qlc-conn cur nil)
        ;; forget what the other end has been told. The writer reconnects on
        ;; its own, and without this the track is never resent: QLC+ comes back
        ;; up and gets beat positions for a track it has no waveform or flags
        ;; for. Only when THIS connection went (runde 197): an old writer's
        ;; error forced a full resend on a healthy one.
        (reset! qlc-last-track {})
        (reset! qlc-sent-track nil)
        (when (bound? #'qlc-next-sent) (reset! qlc-next-sent nil))
        (when (bound? #'qlc-mixing) (reset! qlc-mixing false)))))))

(defn qlc-writer! []
  (or (:writer @qlc-conn)
      (when (> (- (System/currentTimeMillis) @qlc-connect-failed-at) 2000)
      (locking qlc-conn
        (or (:writer @qlc-conn)
            (try
              ;; med timeout: paa en QLC+ der staar paa en anden maskine og er
              ;; slukket blokerer et connect uden timeout i titusinder af
              ;; sekunder - og det sker mens vi holder laasen, saa alle andre
              ;; traade der vil sende staar i koe bag den
              ;; runde 207: no Nagle - a beat is one small line, and the
              ;; status echo right behind it waited for QLC+'s delayed ACK
              ;; (40-200 ms by OS) before it left. And UTF-8 by name: the
              ;; JVM's default charset is not UTF-8 everywhere, QLC+ reads
              ;; UTF-8, and a title with an ø failed to parse there - the
              ;; whole track message dropped without a word.
              (let [sock (doto (Socket.)
                           (.setTcpNoDelay true)
                           ;; runde 247: 300 ms, not 2000 - QLC+ is on this
                           ;; machine, and a refused connect on Windows waits
                           ;; out the whole timeout
                           (.connect (java.net.InetSocketAddress.
                                       ^String qlc-host (int qlc-port)) 300))
                    wr   (PrintWriter. (java.io.OutputStreamWriter. (.getOutputStream sock) "UTF-8") true)]
                ;; runde 247: a NEW connection starts clean, before anyone can
                ;; see it. What queued up while QLC+ was away is stale (up to a
                ;; minute of mix profiles, stops, old NEXTs), and what BLT
                ;; thinks it has told QLC+ is not true for a QLC+ that just came
                ;; up: a running mix was never announced again and NEXT stayed
                ;; empty. Forgetting it makes the next updates send it all.
                (.clear ^java.util.concurrent.ArrayBlockingQueue qlc-out)
                (reset! qlc-last-track {})
                (reset! qlc-sent-track nil)
                (when (bound? #'qlc-next-sent) (reset! qlc-next-sent nil))
                (when (bound? #'qlc-mixing) (reset! qlc-mixing false))
                (when (bound? #'qlc-mix-candidate) (reset! qlc-mix-candidate nil))
                (reset! qlc-connect-failed-at 0)
                (reset! qlc-conn {:socket sock :writer wr})
                (qlc-start-reader! sock)
                (timbre/info "QLC+ connected on" qlc-host qlc-port)
                wr)
              (catch Exception _
                (reset! qlc-connect-failed-at (System/currentTimeMillis))
                nil)))))))

(defonce qlc-out
  ;; En bunden koe og én skrivetraad. PrintWriter.println BLOKERER naar
  ;; modtagerens buffer loeber fuld, og den blev kaldt direkte fra BLT's
  ;; beat-callback - saa en QLC+ der hang tog hele traaden med sig for resten
  ;; af aftenen, uden en fejl og uden en linje i loggen. Nu er det vaerste der
  ;; kan ske, at koen loeber fuld og vi smider en besked vaek.
  (java.util.concurrent.ArrayBlockingQueue. 256))

(defonce qlc-writer-thread
  (doto (Thread.
          (fn []
            (loop []
              ;; ALT pakket ind. Traaden er defonce'et, saa doer den, kommer
              ;; den aldrig igen - koen loeber fuld og hver eneste besked
              ;; bliver droppet resten af aftenen, uden en fejl nogen steder.
              (try
              (let [m (try (.take ^java.util.concurrent.ArrayBlockingQueue qlc-out)
                           (catch InterruptedException _ nil))]
                (when m
                  ;; forbindelsen og dens skriver fra SAMME opslag (runde 197):
                  ;; c laest efter wr kunne vaere nyere end wr, og en fejl paa
                  ;; den gamle skriver rev saa den friske forbindelse ned
                  ;; runde 218: after qlc-writer! the connection is read
                  ;; again, and it is only OURS if it still carries wr - the
                  ;; trigger thread's qlc-send-track! can have made a newer
                  ;; one in between. Not ours (or nil, which qlc-disconnect!
                  ;; reads as "close whatever is there"): leave it, the reader
                  ;; closes a dead socket on its own.
                  (let [c0 @qlc-conn
                        wr (or (:writer c0) (qlc-writer!))
                        c  (if (:writer c0) c0
                               (let [c1 @qlc-conn] (when (identical? (:writer c1) wr) c1)))]
                    (when wr
                      (try
                        (.println ^java.io.PrintWriter wr (json/generate-string m))
                        (when (and c (.checkError ^java.io.PrintWriter wr))
                          (qlc-disconnect! c))
                        (catch Exception _ (when c (qlc-disconnect! c))))))))
              (catch Throwable _ nil))
              (recur)))
          "qlc-writer")
    (.setDaemon true)
    (.start)))

(defn qlc-send!
  "Laeg beskeden i koen. Returnerer false hvis koen er fuld eller QLC+ ikke er
   der - aldrig blokerende, uanset hvad den anden ende laver."
  [m]
  ;; Ingen qlc-writer! her: den forbinder, og et connect blokerer - under en
  ;; laas - i op til to sekunder. Skrivetraaden ejer forbindelsen.
  (or (.offer ^java.util.concurrent.ArrayBlockingQueue qlc-out m)
      (do (try (timbre/warn "QLC+ koen er fuld - besked droppet")
               (catch Throwable _ nil))
          false)))

;; R465_HELLO (Tobias 10-15: "checke 'live' om programmet koerer ... 4 og 5 sekunders
;; intervaller"): BLT siger hej hvert 4. sekund, saa QLC+ ser den foer det foerste
;; nummer - den forbandt kun, naar den havde et nummer eller et slag at sende. En lille
;; linje paa den samme forbindelse; QLC+ ignorerer den (ukendt evt). Kun naar koen er
;; naesten tom: hej venter aldrig foran et slag. Daemon og defonce: en Update starter
;; ikke en til.
(defonce qlc-hello-thread
  (doto (Thread.
          (fn []
            (loop []
              (try
                (Thread/sleep 4000)
                (when (< (.size ^java.util.concurrent.ArrayBlockingQueue qlc-out) 4)
                  (qlc-send! {:evt "hello"}))
                (catch InterruptedException _ nil)
                (catch Throwable _ nil))
              (recur)))
          "qlc-hello")
    (.setDaemon true)
    (.start)))

;; ---------------------------------------------------------------- cache
;;
;; Sections are cached per track, so a second play is instant - and when the
;; operator drags a flag in QLC, the correction comes back over the same
;; socket and is remembered for good.

(require 'clojure.edn)
(def qlc-cache-file (str (System/getProperty "user.home") "/.qlc-track-cache.edn"))

;; runde 208 (Tobias: "Laeg den oedelagte cache fil til siden i stedet"): a
;; file that will not read is MOVED ASIDE as .corrupt-<ms>, not left to be
;; overwritten by the next save - which took every hand correction with it.
;; A missing file is simply a first start.
(defn qlc-cache-load []
  (let [f (java.io.File. ^String qlc-cache-file)]
    (if (not (.exists f))
      {}
      (try
        (let [m (clojure.edn/read-string (slurp f))]
          (if (map? m) m (throw (ex-info "not a map" {}))))
        (catch Throwable e
          (let [aside (str qlc-cache-file ".corrupt-" (System/currentTimeMillis))]
            (try (java.nio.file.Files/move (.toPath f) (.toPath (java.io.File. aside))
                                           (into-array java.nio.file.CopyOption []))
                 (catch Throwable _ nil))
            (try (timbre/error "QLC+ cache file could not be read - moved aside to" aside
                               "and starting empty:" (.getMessage e))
                 (catch Throwable _ nil))
            {}))))))

(def qlc-cache (atom (qlc-cache-load)))

(defn qlc-cache-kept?
  "Worth keeping: it was actually played (fader up), or a hand corrected it.
   Tracks that were only loaded and analysed for NEXT stay in memory only."
  [e]
  ;; false, som i qlc-cache-put!. To forskellige defaults for det samme felt
  ;; er en fejl der venter paa at ske.
  (or (:manual e) (:played e false)))

(defn qlc-cache-save! []
  ;; several threads save (reader, trigger, prepare futures): one at a time.
  ;; Written beside the real file and moved into place: spit truncates first,
  ;; so a crash halfway through used to leave a torn file, and the next start
  ;; read it, threw, and began from {} - every hand correction gone, silently.
  (try (locking qlc-cache-file
         (let [tmp (str qlc-cache-file ".tmp")]
           (spit tmp (pr-str (into {} (filter (fn [[_ e]] (qlc-cache-kept? e)) @qlc-cache))))
           (java.nio.file.Files/move
             (.toPath (java.io.File. tmp))
             (.toPath (java.io.File. ^String qlc-cache-file))
             ;; ATOMIC_MOVE hvor filsystemet kan: uden den kan en stroemafbrydelse
             ;; midt i flytningen efterlade en halv fil, og det er operatoerens
             ;; samlede haandrettelser der ligger i den
             (into-array java.nio.file.CopyOption
                         [java.nio.file.StandardCopyOption/REPLACE_EXISTING
                          java.nio.file.StandardCopyOption/ATOMIC_MOVE]))))
       (catch java.nio.file.AtomicMoveNotSupportedException _
         ;; nogle filsystemer kan ikke - saa hellere en almindelig flytning
         ;; end slet ingen cache
         (try (locking qlc-cache-file
                (java.nio.file.Files/move
                  (.toPath (java.io.File. (str qlc-cache-file ".tmp")))
                  (.toPath (java.io.File. ^String qlc-cache-file))
                  (into-array java.nio.file.CopyOption
                              [java.nio.file.StandardCopyOption/REPLACE_EXISTING])))
              (catch Throwable e (timbre/warn e "QLC+ cache not saved"))))
       (catch Throwable e (timbre/warn e "QLC+ cache not saved"))))

(defn qlc-cache-get
  "Cached markers for a title, or nil. Automatic entries from an older
   analysis are ignored; manual corrections always count. With a duration in
   hand we also check it: two tracks can carry the same artist and title (an
   original and an extended mix the label tagged alike), and the shorter one
   would otherwise be handed flags past its own last beat. Entries written
   before we recorded a duration have none, and are still trusted."
  ([title] (qlc-cache-get title nil))
  ([title ms]
   (when-let [e (get @qlc-cache title)]
     ;; rekordbox' faser er rekordbox' - vores versionsnummer siger intet om
     ;; dem (runde 195: v8 smed ellers alle gemte faser vaek)
     (when (and (or (:manual e) (:rekordbox e) (= (:version e) qlc-analysis-version))
                (or (nil? ms) (nil? (:duration e))
                    (< (Math/abs (- (long ms) (long (:duration e)))) 3000)))
       (:markers e)))))

(defn qlc-cache-put!
  "Store markers for a title. An automatic set never overwrites a hand-made
   correction; a manual set always wins. The guard sits inside the swap!, so
   two threads (the socket reader and a prepare future) cannot both read
   'not manual' and have the automatic one land last."
  ([title markers manual?] (qlc-cache-put! title markers manual? nil))
  ([title markers manual? ms]
   ;; Vask dem foerst. Markoerer der kommer tilbage fra QLC+ gik lige ind i
   ;; cachen og ud igen uden et eneste tjek - og der ligger allerede numre paa
   ;; disken med to flag paa samme beat og med flag i forkert raekkefoelge.
   ;; Reglen er "typen paa den seneste markoer <= B", og den er udefineret
   ;; naar to markoerer staar paa samme beat.
   ;; analyse 10's kick path (qlc-auto-markers marks it, either way). A set
   ;; without the mark - QLC+'s UNDO back to the analysis, a hand edit - keeps
   ;; the entry's own (10c): an edit and an UNDO made a kick analysis "9" for
   ;; good, and QLC+'s second pass then merged its short sections away
   (let [kick-m  (meta markers)
         markers (->> (if (coll? markers) markers [])
                      (filter map?)
                      (keep (fn [x]
                              (let [b (:beat x)]
                                (when (and (number? b) (>= (long b) 1))
                                  (assoc x :beat (long b))))))
                      (filter #(contains? #{"break" "build" "drop" "normal"
                                            "drive" "intro" "outro"} (:type %)))
                      (sort-by :beat)
                      (partition-by :beat)
                      (map last)
                      vec)]
   (when (and title (or manual? (seq markers)))
     (let [before @qlc-cache
           after  (swap! qlc-cache
                         (fn [c]
                           (let [old (get c title)]
                             ;; rekordbox' faser gemt af en aeldre version som
                             ;; :manual er ikke en haandrettelse (qlc-hand-made?) -
                             ;; analyse 10 maa skrive over dem
                             (if (or manual? (not (:manual old)) (:rekordbox old))
                               (assoc c title
                                      {:markers  (vec markers)
                                       :manual   (boolean manual?)
                                       ;; a correction is made on the track that plays
                                       ;; false, ikke true: entries fra en aeldre version har ingen :played, og
     ;; med true blev hvert eneste nummer der bare blev lagt paa et daek
     ;; skrevet til disken for altid - praecis det qlc-cache-kept? findes for
     ;; at undgaa
     :played   (boolean (or manual? (if old (:played old false) false)))
                                       ;; en haandrettelse er lavet paa det nummer der koerer,
                                       ;; saa den skal ogsaa staa i tidslinjen
                                       :played-at (or (:played-at old)
                                                      (when manual? (System/currentTimeMillis)))
                                       :duration (or ms (:duration old))
                                       :kick     (if (contains? kick-m :kick)
                                                   (boolean (:kick kick-m))
                                                   (boolean (:kick old)))
                                       :version  qlc-analysis-version
                                       ;; B22: a new analysis says its grid;
                                       ;; any other write keeps the entry's
                                       :grid     (if (contains? kick-m :grid)
                                                   (:grid kick-m)
                                                   (:grid old))})
                               c))))]
       ;; the file holds only kept entries (played or hand-made): a change to
       ;; one that is neither leaves it as it was, and each new track wrote it
       ;; two or three times over for nothing, on the status thread (runde 207)
       (when (and (not= before after)
                  (or (qlc-cache-kept? (get before title)) (qlc-cache-kept? (get after title))))
         (qlc-cache-save!)))))))

(defn qlc-with-grid
  "B22: the beat-grid signature rides on the markers' metadata into
   qlc-cache-put!, so it is written in the same swap! and saved with the
   entry (a note! after the put! was never saved, and every later put! -
   QLC+'s second pass, a hand edit, an UNDO - dropped it)."
  [m gsig]
  (if (instance? clojure.lang.IObj m) (vary-meta m assoc :grid gsig) m))

(defn qlc-cache-note!
  "Skriv et felt paa en entry der allerede findes. assoc-in ville ellers
   OPRETTE en entry uden :markers og uden :version, som qlc-cache-get aldrig
   rammer og qlc-cache-kept? beholder for evigt."
  [title k v]
  (when title
    (swap! qlc-cache (fn [c] (if (contains? c title) (assoc-in c [title k] v) c)))))

;; En laeselig liste over hvad der faktisk blev spillet, skrevet i samme
;; oejeblik nummeret krydser taersklen. Tobias, 2026-09-17: "gem det i en
;; fil, eller en eksport knap, hvad end der er nemmest." Det her er det
;; nemmeste OG det sikreste: der er ingen knap at huske, filen er opdateret
;; hele natten, og et strømsvigt koster hoejst det sidste nummer.
(def qlc-played-log (str (System/getProperty "user.home") "/qlc-played.csv"))

(defn qlc-played-log! [at title]
  ;; Append, ikke spit: hele natten skal staa der, ikke det sidste nummer.
  ;; Semikolon som skilletegn - en titel indeholder tit et komma, og her er
  ;; ingen grund til at finde citationstegn frem. Dato og klokkeslet hver for
  ;; sig, saa den kan sorteres og grupperes uden at blive parset.
  (try
    (locking qlc-played-log
      (let [f   (java.io.File. ^String qlc-played-log)
            new? (not (.exists f))
            d   (java.util.Date. ^long at)
            day (.format (java.text.SimpleDateFormat. "yyyy-MM-dd") d)
            clk (.format (java.text.SimpleDateFormat. "HH:mm:ss") d)]
        (when new?
          (spit f "dato;klokken;titel\n" :append true))
        ;; Titlen paa EEN linje, uden skilletegn. Semikolon bliver komma, og
        ;; linjeskift bliver mellemrum: rekordbox-titler indeholder dem -
        ;; "... - DO 4 LOVE - Onderkoffer Remix\n(DJcity Intro)" delte sig i
        ;; to linjer og gjorde begge halvdele ulaeselige. Fire oedelagte
        ;; linjer i loggen fra 20-09, alle fra det ene nummer. Motoren fik
        ;; den samme rettelse paa sin tracklog i runde 95; den her blev
        ;; glemt.
        (spit f (str day ";" clk ";"
                     (-> (str title)
                         (clojure.string/replace #"[\r\n]+" " ")
                         (clojure.string/replace ";" ",")
                         clojure.string/trim)
                     "\n")
              :append true)))
    (catch Throwable e (timbre/warn e "QLC+ played log not written"))))

(defn qlc-cache-mark-played!
  "Marker en entry som spillet, med tidspunktet. :played-at er det listen
   sorteres paa - uden den kan QLC+ kun vise cachen alfabetisk."
  [title at]
  ;; every play, not the first: the list is newest first, and a track played
  ;; tonight stood under the date it was first played weeks ago (runde 207).
  ;; qlc-note-played! calls this once per play (:logged). The save runs off
  ;; the status thread - it is a whole-file write under a lock.
  (when (and title (contains? @qlc-cache title))
    (swap! qlc-cache (fn [c] (if (contains? c title)
                               (update c title assoc :played true :played-at at)
                               c)))
    (future (qlc-cache-save!))))

(def qlc-mixer-seen (atom false))   ; a DJM reports on-air; without one "playing" has to do
;; runde 197: decks playing for a minute with none on air = the mixer stopped
;; reporting (a guest's mixer, a pulled cable) - "seen" was forever, and from
;; then nothing counted as played, no mix, no profile
(def qlc-mixer-quiet-since (atom nil))
;; runde 197: the title last written to the played list, per deck - a pause, a
;; fader dip or a metadata blip dropped :logged and the same play was listed again
(defonce qlc-last-logged (atom {}))

;; Hvor laenge et nummer skal have koert paa luften foer det taeller som
;; spillet. Tobias, 2026-09-17: "kun cache dem der faktisk bliver spillet og
;; ikke ALLE der bliver loaded."
;;
;; qlc-note-played! kaldes ved hvert eneste beat, og foer i dag markerede den
;; et nummer som spillet efter ET beat. Finder man et cue-punkt, eller lader
;; man to takter loebe for at hoere hvor man er, laa nummeret paa disken for
;; evigt. Og har BLT ikke set en DJM, taeller ethvert spillende deck - ogsaa
;; det man hoerer i hovedtelefonerne.
;;
;; Tredive sekunder i TRAEK, paa samme titel. Stopper afspilningen, eller
;; skiftes nummeret, begynder uret forfra.
(def qlc-played-seconds 30)
(def qlc-play-since (atom {}))      ; player -> {:title t :since ms}

(defn qlc-note-played!
  "Et deck der har spillet paa luften i qlc-played-seconds i traek har
   spillet nummeret: dets analyse er vaerd at gemme."
  []
  (try
    (let [vc  (org.deepsymmetry.beatlink.VirtualCdj/getInstance)
          now (System/currentTimeMillis)
          any-air  (volatile! false)
          any-play (volatile! false)]
      (doseq [p [1 2 3 4]]
        (let [st      (.getLatestStatusFor vc (int p))
              title   (when st (qlc-title p))
              on-air  (boolean (when st (try (.isOnAir st) (catch Throwable _ false))))
              playing (boolean (when st (.isPlaying st)))]
          (when on-air (reset! qlc-mixer-seen true) (vreset! any-air true))
          (when playing (vreset! any-play true))
          ;; en anden titel paa decket: den naeste afspilning maa logges
          (when (and title (not= title (get @qlc-last-logged p)))
            (swap! qlc-last-logged dissoc p))
          (if (and st playing title (or on-air (not @qlc-mixer-seen)))
            (let [cur (get @qlc-play-since p)]
              (if (= (:title cur) title)
                (when (and (not (:logged cur))
                           (>= (- now (:since cur 0)) (* 1000 qlc-played-seconds)))
                  ;; :logged pr. AFSPILNING, ikke pr. titel. qlc-cache-mark-played!
                  ;; goer intet anden gang, saa uden det her ville et nummer der
                  ;; koeres to gange paa en aften kun staa i listen én gang - og
                  ;; saa er filen ikke en playliste.
                  (swap! qlc-play-since assoc-in [p :logged] true)
                  (qlc-cache-mark-played! title now)
                  (when (not= title (get @qlc-last-logged p))
                    ;; the other decks' marks go: a real second play of this
                    ;; title later, after other tracks, is listed again (runde 198)
                    ;; keep the marks of decks still on air (a mix: A's fader
                    ;; dip must not list A twice) - drop only the rest (r199)
                    (reset! qlc-last-logged
                            (assoc (select-keys @qlc-last-logged (keys @qlc-play-since)) p title))
                    (qlc-played-log! now title)))
                (swap! qlc-play-since assoc p {:title title :since now :logged false})))
            (swap! qlc-play-since dissoc p))))
      ;; the mixer's presence ages (runde 197)
      (cond
        @any-air (reset! qlc-mixer-quiet-since nil)
        (not @any-play) (reset! qlc-mixer-quiet-since nil)
        (nil? @qlc-mixer-quiet-since) (reset! qlc-mixer-quiet-since now)
        (and @qlc-mixer-seen (> (- now @qlc-mixer-quiet-since) 60000))
        (do (reset! qlc-mixer-seen false)
            (timbre/info "QLC+: no on-air from the mixer for a minute - counting playing decks"))))
    (catch Throwable _ nil)))

(defn qlc-send-cache-list! []
  ;; Nyeste foerst, ikke alfabetisk: listen er en tidslinje over hvad der
  ;; blev spillet, og den man leder efter er som regel fra i aften. Entries
  ;; fra foer :played-at fandtes har ingen tid - de faar 0 og lander nederst
  ;; under deres egen overskrift i QLC+, med markoererne i behold.
  (qlc-send! {:evt    "cache"
              :tracks (->> @qlc-cache
                           (filter (fn [[_ e]] (qlc-cache-kept? e)))
                           (map (fn [[title e]] {:title     title
                                                 :flags     (count (:markers e))
                                                 ;; en haandrettelse, ikke rekordbox' faser (runde 196)
                                                 :manual    (qlc-hand-made? e)
                                                 :version   (:version e)
                                                 :played-at (or (:played-at e) 0)}))
                           (sort-by (juxt (comp - :played-at) :title))
                           vec)}))

;; ---------------------------------------------------------------- mixing + next

;; defined further down - Clojure wants to know they exist
(declare qlc-curves qlc-build-markers qlc-cue-beats qlc-title
         ;; Clojure slaar navne op paa kompileringstidspunktet, og de her
         ;; staar laengere nede i filen end det foerste sted de bruges
         qlc-target-sections qlc-patent-for qlc-downbeat)

(defonce qlc-mixing (atom false))   ; defonce (fejljagt 3): a reload kept QLC+ on a mix BLT had forgotten
(def qlc-prepared (atom #{}))
;; runde 197: a title whose analysis threw waits 30 s before the next try, and
;; a failed send of the tracked deck 5 s - both were retried 5x a second, a
;; full analysis and a stack trace each time
(def qlc-failed-at (atom {}))

(defn qlc-mixing?
  "Two or more players playing (and on air, when the mixer tells us) - a
   transition, not a track. The light should hold its colour and stay calm."
  []
  (try
    (let [vc (org.deepsymmetry.beatlink.VirtualCdj/getInstance)
          st (keep #(.getLatestStatusFor vc (int %)) [1 2 3 4])
          live (filter #(and (.isPlaying %) (try (.isOnAir %) (catch Throwable _ false))) st)
          playing (filter #(.isPlaying %) st)]
      ;; with a mixer in the network only on-air decks count; two decks cued
      ;; in the headphones are not a mix
      (>= (count (if @qlc-mixer-seen live playing)) 2))
    (catch Throwable _ false)))

;; runde 197: a mix is a mix once it has held - two seconds in, four out. Every
;; play-tap while cueing, every fader dip sent a "mix", and QLC+ restarted the
;; outgoing track's fade on each one
(defonce qlc-mix-candidate (atom nil))   ; [value since-ms]

(defn qlc-update-mixing! []
  (let [m   (qlc-mixing?)
        now (System/currentTimeMillis)
        [cv since] @qlc-mix-candidate]
    (cond
      (= m @qlc-mixing) (reset! qlc-mix-candidate nil)
      (not= cv m)       (reset! qlc-mix-candidate [m now])
      (>= (- now since) (if m 2000 4000))
      ;; only when it went: a full queue dropped it, and "mix" was never sent
      ;; again until the mix was over (fejljagt 3) - the next update retries
      (when (qlc-send! {:evt "mix" :mixing m})
        (reset! qlc-mixing m)
        (reset! qlc-mix-candidate nil)))))

(def qlc-loaded-at (atom {}))   ; player -> [title, ms when we first saw it]
(defonce qlc-next-sent (atom nil))  ; the title QLC+ currently shows as NEXT (defonce: a reload left NEXT on screen, fejljagt 3)

(defn qlc-key
  "The track's key as rekordbox wrote it (\"Am\", \"8A\", \"1m\" - the label
   depends on the rekordbox preference), or nil when the metadata has none.
   QLC+ leans its palette on it: minor cold, major warm; nil leans nowhere."
  [player]
  (try
    (let [mf (org.deepsymmetry.beatlink.data.MetadataFinder/getInstance)
          md (.getLatestMetadataFor mf (int player))
          k  (when md (.getKey md))
          s  (when k (.-label k))]
      (when (seq s) s))
    (catch Throwable _ nil)))

(defn qlc-genre
  "R493_A1_BLT_GENRE (runde 493): the track's genre as rekordbox has it
   (\"Afro House\"), or nil when the metadata has none. Only a weak hint for
   QLC+'s genre profile - Tobias: \"nogle, men det er bestemt ikke altid
   korrekt\". Read from BLT's metadata cache, like the key: nothing is asked
   of or written to the CDJs."
  [player]
  (try
    (let [mf (org.deepsymmetry.beatlink.data.MetadataFinder/getInstance)
          md (.getLatestMetadataFor mf (int player))
          g  (when md (.getGenre md))
          s  (when g (.-label g))]
      (when (seq s) s))
    (catch Throwable _ nil)))

(defn qlc-guess-next
  "Which of the other decks goes on next? Playing but not on air (the DJ is
   beatmatching in the headphones) beats loaded-and-stopped; among those the
   most recently loaded wins. Returns [player title] or nil."
  [tracked]
  (try
    (let [vc  (org.deepsymmetry.beatlink.VirtualCdj/getInstance)
          now (System/currentTimeMillis)
          cands (for [p [1 2 3 4]
                      :when (not= (int p) (int tracked))
                      :let [title (qlc-title p)
                            st    (.getLatestStatusFor vc (int p))]
                      :when (and title st)]
                  (let [[old-title at] (get @qlc-loaded-at p)
                        at (if (= old-title title) at now)]
                    (swap! qlc-loaded-at assoc p [title at])
                    {:player p :title title :at at
                     :playing (.isPlaying st)
                     :on-air  (try (.isOnAir st) (catch Throwable _ false))}))
          ;; a deck already in the mix is not "next". Without a DJM nothing
          ;; reports on-air, so "playing" has to stand in for it - otherwise
          ;; the deck that is playing out gets the top rank and is announced
          ;; as the next track
          live? (fn [c] (if @qlc-mixer-seen (:on-air c) (:playing c)))
          cands (remove #(and (:playing %) (live? %)) cands)
          score (fn [c] [(if (and (:playing c) (not (live? c))) 1 0) (:at c)])
          best  (last (sort-by score cands))]
      (when best [(:player best) (:title best)]))
    (catch Throwable _ nil)))

(defn qlc-send-mix-profile!
  "Only a single other LIVE deck may shape a handover. Loaded/cued guesses
   never count. Ambiguous mixes explicitly invalidate the previous profile."
  [tracked]
  (try
    (let [vc (org.deepsymmetry.beatlink.VirtualCdj/getInstance)
          live (for [p [1 2 3 4]
                     :when (and tracked (not= (int p) (int tracked)))
                     :let [st (.getLatestStatusFor vc (int p))]
                     :when (and st (.isPlaying st)
                                (or (not @qlc-mixer-seen)
                                    (try (.isOnAir st) (catch Throwable _ false))))]
                 [p st])
          [p st] (when (and @qlc-mixing (= 1 (count live))) (first live))
          title (when p (qlc-title p))
          beat (when st (.getBeatNumber st))
          markers (when title (qlc-cache-get title))
          marker (when (and beat (pos? beat) markers)
                   (last (sort-by :beat (filter #(<= (:beat %) beat) markers))))]
      (qlc-send! (if marker
                   {:evt "mix-profile" :title title :state (:type marker)
                    :energy (get marker :energy -1.0)}
                   {:evt "mix-profile" :title "" :state "" :energy -1.0})))
    (catch Throwable _ nil)))

;; defineret laengere nede, under rekordbox-afsnittet
(declare qlc-hand-made? qlc-phrase-markers qlc-auto-markers)
(declare qlc-ml-for! qlc-ml-known)   ; R525_DECL: de laerte sektioner (laengere nede)
(declare qlc-ml-hand-later!)          ; R528_HANDDECL: hans rettelser som traeningsdata (laengere nede)

(defn qlc-prepare-others!
  "Analyse what is loaded on the other decks now, so it is in the cache the
   moment it goes on air - and tell QLC+ which one is probably next."
  [tracked]
  (doseq [p [1 2 3 4] :when (and tracked (not= (int p) (int tracked)))]
    (try
      (let [wf     (org.deepsymmetry.beatlink.data.WaveformFinder/getInstance)
            bg     (org.deepsymmetry.beatlink.data.BeatGridFinder/getInstance)
            detail (.getLatestDetailFor wf (int p))
            grid   (.getLatestBeatGridFor bg (int p))
            title  (when (and detail grid) (qlc-title p))]
        ;; swap-vals!: to triggers kunne begge laese "ikke klargjort" og begge
        ;; starte en fuld analyse af det samme nummer
        (when (and title
                   (> (- (System/currentTimeMillis) (get @qlc-failed-at title 0)) 30000)
                   (not (contains? (first (swap-vals! qlc-prepared conj title))
                                   title)))
          (future
            (try
              ;; med laengden: uden den kan cachen ikke se forskel paa
              ;; en Original og en Extended Mix med samme titel, og
              ;; klargoeringen vinder naesten altid kaploebet med
              ;; afspilningen - saa vagten virkede aldrig
              (let [ms     (try (.getTotalTime detail) (catch Throwable _ nil))
                    cached (qlc-cache-get title ms)
                    entry  (get @qlc-cache title)
                    ;; runde 295 (T2): the grid, as in qlc-send-track! - else a
                    ;; prepared set is thrown away at play
                    gsig   [(.-beatCount grid) (qlc-downbeat grid (.-beatCount grid))]]
                ;; en haandrettelse og vores egen analyse i denne version
                ;; staar; alt andet regnes om, som i qlc-send-track!.
                ;; ask? true: det her er en future, saa den maa spoerge
                ;; afspilleren direkte efter rekordbox' fraselinjer.
                ;; R525_PREP: de laerte sektioner foerst (kun i hukommelsen) - saa er de klar
                ;; naar NEXT sendes, og naar nummeret spilles
                (qlc-ml-for! title grid gsig #(qlc-curves detail grid))
                (when-not (and cached
                               (or (qlc-hand-made? entry)
                                   (and (not (:rekordbox entry))
                                        (= (:version entry) qlc-analysis-version)
                                        (= (:grid entry) gsig))))
                  (qlc-cache-put! title
                                  (qlc-with-grid
                                   (qlc-auto-markers p detail grid (qlc-curves detail grid) true)
                                   gsig)
                                  false ms)))
              (timbre/info "QLC+ prepared" title "on player" p)
              (catch Throwable e
                (swap! qlc-prepared disj title)     ; try again - in 30 s
                (swap! qlc-failed-at assoc title (System/currentTimeMillis))
                (timbre/warn e "QLC+ prepare failed"))))))
      (catch Throwable _ nil)))
  ;; the guess: only sent when it changes and its analysis is ready
  (try
    (let [[p title] (qlc-guess-next tracked)
          e         (when title (get @qlc-cache title))
          grid      (when title (.getLatestBeatGridFor (org.deepsymmetry.beatlink.data.BeatGridFinder/getInstance) (int p)))
          ;; B22: the grid too, as qlc-send-track! checks it - a re-gridded
          ;; track went out as NEXT with its old flags, and NEXT is sent once
          gsig      (when grid [(.-beatCount grid) (qlc-downbeat grid (.-beatCount grid))])
          ;; a hand correction or this version's analysis - not rekordbox'
          ;; phrases from before analyse 10: NEXT is sent once per title, and
          ;; the prepared v10 set would never replace them (analyse 10)
          markers   (when (and e (or (qlc-hand-made? e)
                                     (and (not (:rekordbox e)) (= (:version e) qlc-analysis-version)
                                          (some? gsig) (= (:grid e) gsig))))
                      (qlc-cache-get title))]
      (cond
        (and title markers (not= title @qlc-next-sent))
        ;; noted as shown only when it went (fejljagt 3): a full queue
        ;; dropped it, and the same NEXT was never tried again
        (when (qlc-send! {:evt "next" :title title
                          :key (qlc-key p)
                          :beats (if grid (.-beatCount grid) 0)
                          :markers markers
                          ;; R525_NEXTAI: de laerte sektioner, naar klargoeringen har regnet dem
                          :markersAi (or (when-not (qlc-hand-made? e) (qlc-ml-known title gsig)) [])})
          (reset! qlc-next-sent title))
        (and (nil? title) @qlc-next-sent)
        (when (qlc-send! {:evt "next" :title "" :beats 0 :markers []})
          (reset! qlc-next-sent nil))))
    (catch Throwable _ nil)))

(defn qlc-handle-line!
  "A line from QLC+. Today that is only marker corrections."
  [line]
  (try
    (let [m (json/parse-string line true)]
      (case (:evt m)
        "markers"
        ;; QLC+ says whether these are the operator's (manual) or its own
        ;; second pass over our analysis (automatic, never overwrites manual)
        (let [manual? (if (contains? m :manual) (boolean (:manual m)) true)
              deck-ms (let [[p _] @qlc-sent-track]
                        (when (and p (= (:title m) (qlc-title p)))
                          (try (.getTotalTime (.getLatestDetailFor
                                                (org.deepsymmetry.beatlink.data.WaveformFinder/getInstance) (int p)))
                               (catch Throwable _ nil))))
              e0      (get @qlc-cache (:title m))
              ;; another edit of this title: its automatic set is no UNDO of that correction
              other?  (boolean (and deck-ms (:duration e0)
                                    (>= (Math/abs (- (long deck-ms) (long (:duration e0)))) 3000)))]
          ;; an automatic set from QLC+ (an UNDO back to the analysis) has to
          ;; be able to clear the manual mark, or the entry stays a
          ;; "correction" for good and no later analysis is ever used
          (when (and (not manual?) (not other?) (:title m) (contains? @qlc-cache (:title m)))
            (qlc-cache-note! (:title m) :manual false)
            ;; save here too: if the undone set is empty, qlc-cache-put! below
            ;; bails and never saves, and the cleared mark would be lost on
            ;; the next restart - which is the whole bug this was meant to fix
            (qlc-cache-save!))
          ;; an UNDO back to rekordbox' own phrases arrives as manual (the
          ;; phrase set came to QLC+ as manual): the same flags are not a
          ;; correction, and storing them as one made the phrases a permanent
          ;; "hand correction" (runde 198)
          (let [e    (get @qlc-cache (:title m))
                sig  (fn [ms] (vec (sort (map (fn [x] [(long (:beat x)) (str (:type x))]) ms))))
                same (and manual? (:rekordbox e)
                          (= (sig (:markers e)) (sig (:markers m))))]
            (when-not same
              ;; with the length of the track they were made on, when it is the
              ;; one on the deck QLC+ has (runde 207): stored without one, a
              ;; correction to an Extended mix kept the Original's duration -
              ;; the Extended then failed the length check and was analysed
              ;; afresh, and the Original got the Extended's flags
              (qlc-cache-put! (:title m) (:markers m) manual?
                              (let [[p _] @qlc-sent-track]
                                (when (and p (= (:title m) (qlc-title p)))
                                  (try (.getTotalTime
                                         (.getLatestDetailFor
                                           (org.deepsymmetry.beatlink.data.WaveformFinder/getInstance)
                                           (int p)))
                                       (catch Throwable _ nil)))))
              (timbre/info "QLC+ markers" (if manual? "corrected" "refined") "for" (:title m))
              ;; R528_HAND_LIVE: hans rettelse paa nummeret der spiller -> traeningsdata
              (when manual? (qlc-ml-hand-later! (:title m) (:markers m))))))

        ;; the cache editor on QLC+'s SETUP page
        "cache-list"
        (qlc-send-cache-list!)

        "cache-forget"
        (do (swap! qlc-cache dissoc (:title m))
            (swap! qlc-prepared disj (:title m))
            (qlc-cache-save!)
            (qlc-send-cache-list!)
            (timbre/info "QLC+ forgot" (:title m)))

        "cache-forget-auto"
        ;; rekordbox-faser er ogsaa automatiske - kun haandrettelser bliver (runde 196)
        (do (swap! qlc-cache (fn [c] (into {} (filter (fn [[_ e]] (qlc-hand-made? e)) c))))
            (reset! qlc-prepared #{})
            (qlc-cache-save!)
            (qlc-send-cache-list!)
            (timbre/info "QLC+ forgot every automatic analysis"))

        nil))
    (catch Throwable e (timbre/warn e "QLC+ bad line"))))

(defn qlc-start-reader! [sock]
  (future
    (try
      (with-open [rdr (BufferedReader. (InputStreamReader. (.getInputStream sock) "UTF-8"))]   ; runde 207
        (loop []
          (when-let [line (.readLine rdr)]
            (qlc-handle-line! line)
            (recur))))
      (catch Throwable _ nil))
    ;; only THIS connection (runde 207): between the check and the call a new
    ;; one could have come up, and the bare call closed that instead
    (qlc-disconnect! {:socket sock})))

(defn qlc-test! []
  (qlc-send! {:evt "track" :title "TEST FROM BLT" :bpm 128.0 :beats 64
              :waveform (vec (repeatedly 64 #(rand-int 256)))
              :markers [{:beat 17 :type "break"} {:beat 33 :type "drop"}]})
  (qlc-send! {:evt "pos" :beat 1 :playing true}))

;; ---------------------------------------------------------------- tuning

(def qlc-tuning
  (atom {:min-section-bars 8      ; korteste sektion
         :max-section-bars 32     ; laengste sektion - laengere deles
         :bars-per-section 24     ; sigtepunkt: en sektion pr. saa mange takter
         :build-bars       8      ; build er mindst saa mange takter ...
         :build-max-bars   32     ; ... og hoejst saa mange, mens energien stiger
         :build-rise       0.03   ; ... og energien SKAL stige saa meget, ellers
                                  ;     er der intet build foran drop'et
         :smooth-beats     4      ; glatning: en takt
         :edge-min-bars    4      ; foerste/sidste sektion maa vaere saa kort (intro/outro)
         :force-step       0.35   ; energispring over dette bliver ALTID en graense
         :abs-quiet        0.25   ; energi under dette er ALTID break, uanset kick
         ;; ... men en absolut taerskel kan ligge UNDER hele nummerets
         ;; interval, og saa fanger den ingenting. To numre 2026-09-17
         ;; spaendte 34-81 % og 42-76 % og blev NORMAL hele vejen. Derfor
         ;; ogsaa relativt: den laveste del af nummerets eget spaend.
         :drive-rel        0.70   ; groove over denne del af spaendet = "drive"
         :drop-lift        0.35   ; energiloeft der goer et drop til et drop uden build
         :rel-spread       0.12   ; er spaendet mindre, ER nummeret ensartet - lad det vaere
         :rel-quiet        0.33   ; nederste tredjedel af spaendet = break

         :kick-on          0.40   ; sektionens kick over dette = kick er paa
         :loud-on          0.65   ; ... eller energi over dette (hedge)
         :drop-min         0.60   ; kick tilbage efter break er drop fra denne energi ...
         :drop-rel         0.90   ; ... og mindst saa stor en del af nummerets hoejeste sektion
         :abs-full         0.85   ; energi-fallback: herover ...
         :drop-rise        0.20   ; ... og mindst saa stort loeft = drop
         :grid-trust-min   0.60   ; under dette soeges beat-fasen selv
         :novelty-weight   1.0    ; Foote-novelty i overgangs-scoren
         :repeat-sim       0.97   ; sektioner der ligner hinanden saa meget deles tier ...
         :repeat-energy    0.05   ; ... og ligger saa taet i energi (0.12 loeftede naesten alle grooves til drop)
         :roll-gain        1.30   ; snare-roll skal stige saa meget for at vaere en build
         :refine-bars      2      ; flyt graenser op til saa mange takter
         :refine-gain      1.25   ; ... hvis kick-skiftet der er saa meget staerkere
         :patent-cuts      true   ; Pioneers egen graensedetektor (US10629173B2)
         :patent-margin    0.10   ; ... Q saa langt under nummerets eget loft = graense
         :attack-frac      6      ; attack-vinduet = 1/6 af beatet (~80 ms)
         ;; analyse 10 - sektioner fra kicket (qlc-kick-markers). Maalt mod
         ;; Tobias' haandrettede flag, se kommentaren dér.
         :kick-on-beats    3      ; en kick-takt: saa mange af 4 slag med kick (hoejst) ...
         :kick-drop-rel    0.70   ; kick + energi over denne del af nummerets top = drop, ellers normal (runde 354: 0.75 -> 0.70)
         :kick-block-bars  8      ; en kick-straekning deles i blokke paa saa mange takter (uden rekordbox)
         :kick-blip-bars   2      ; kick i saa faa takter midt i et break er et break
         :kick-min-gap-bars 2     ; et hul i kicket kortere end saa mange takter er et drop-out: det hoerer til kicket (10c)
         :kick-gap-iso-bars 8     ; (kun med :kick-min-gap-bars 1) et hul paa én takt er et break kun hvis intet andet hul er saa taet paa
         :kick-drop-gap-bars 3    ; et hul mellem to drops kortere end saa mange takter er et break, aldrig et build (10c)
         :kick-build-min-bars 2   ; build foer et drop: mindst ...
         :kick-build-max-bars 8   ; ... og hoejst saa mange takter
         :kick-short-on-bars 4    ; kick i saa faa takter mellem et break og et drop = build
         :kick-min-share   0.30   ; kick i under saa stor en del af takterne: ikke et kick-nummer - den gamle analyse (10f)
         ;; EDM (10g): paa et nummer med rekordbox' EDM-form (mood high, Down -> Up -> Chorus)
         ;; siger rekordbox' Chorus hvad der er drop inde i en kick-straekning
         :kick-edm-chorus-floor 0.75  ; en Chorus-blok er drop fra saa stor en del af toppen (runde 354: 0.60 -> 0.75)
         :kick-edm-other-drop   0.97  ; alt andet med kick er normal - under saa hoej energi
         :kick-edm-up-build-bars 8})) ; de sidste saa mange takter af et Up lige foer et Chorus = build

(defn qlc-opt [k dflt] (get @qlc-tuning k dflt))

(def qlc-half-frames-per-ms 0.15)

(defn qlc-min-beats []   (* 4 (qlc-opt :min-section-bars 8)))
(defn qlc-max-beats []   (* 4 (qlc-opt :max-section-bars 32)))
(defn qlc-build-beats [] (* 4 (qlc-opt :build-bars 8)))

;; ---------------------------------------------------------------- hjaelpere

(defn qlc-percentile [coll pct]
  (let [s (vec (sort coll))]
    (if (seq s)
      (nth s (min (dec (count s)) (long (* (/ pct 100.0) (count s)))))
      0.0)))

(defn qlc-normalize-to [curve ref]
  (if (> ref 1e-6)
    (mapv #(min 1.0 (/ % ref)) curve)
    (mapv (constantly 0.0) curve)))

(defn qlc-smooth [curve n]
  (let [v (vec curve) c (count v) half (quot (max 1 n) 2)]
    (mapv (fn [i]
            (let [lo (max 0 (- i half))
                  hi (min c (+ i half 1))
                  sl (subvec v lo hi)]
              (/ (reduce + sl) (count sl))))
          (range c))))

(defn qlc-mean-range
  "Gennemsnit i det 0-indekserede interval [a b)."
  [v a b]
  (let [c (count v) a (max 0 a) b (min c b)]
    (if (< a b) (/ (reduce + (subvec v a b)) (- b a)) 0.0)))

(defn qlc-step
  "Hvor meget kurven skifter ved i, maalt over w beats paa hver side."
  [v i w]
  (Math/abs (- (qlc-mean-range v i (+ i w))
               (qlc-mean-range v (- i w) i))))

;; ---------------------------------------------------------------- maaling

(defn qlc-measure-beat
  "Et beat -> {:level :low :mid :high :kick :peak :roll :q-high :pump}.
   low/mid/high = hoejde vaegtet med roed/groen/blaa andel (bas / mellem / top).
   kick = hvor meget mere bas der er i attack-vinduet end i resten (pulsen).
   peak = hvor i beatet (0..1) bassen topper - bruges til at bedoemme grid'et.
   roll = antal transienter i mellem+top i beatet - stiger i en snare-roll.
   q-high = toppens anslag i hver fjerdedel af beatet (runde 358).
   pump = hvor dybt lyden falder efter beatets anslag (runde 359).
   offset-ms skubber vinduet, naar beatgrid'et ikke er til at stole paa."
  [detail grid beat total frames offset-ms]
  ;; Typehints. Uden dem gaar HVERT eneste opslag i boelgeformen gennem
  ;; refleksion - 50-100 gange langsommere - og det er 60.000 opslag pr.
  ;; nummer, paa den traad der ogsaa skal levere beats.
  (let [^org.deepsymmetry.beatlink.data.WaveformDetail detail detail
        ^org.deepsymmetry.beatlink.data.BeatGrid grid grid
        ms0    (+ (.getTimeWithinTrack grid (int beat)) offset-ms)
        ms1    (if (< beat total)
                 (+ (.getTimeWithinTrack grid (int (inc beat))) offset-ms)
                 (+ ms0 470))
        s0     (max 0 (long (* ms0 qlc-half-frames-per-ms)))
        s1     (min frames (max (inc s0) (long (* ms1 qlc-half-frames-per-ms))))
        n      (max 1 (- s1 s0))
        attack (max 2 (quot n (max 1 (qlc-opt :attack-frac 6))))
        col?   (.-isColor detail)
        band   (fn [i]
                       (let [h (double (.segmentHeight detail (int i) (int 1)))]
                         (if col?
                           ;; RUNDE 319: Beat Links WaveformDetail.segmentColor bygger
                           ;; farven som (roed, BLAA, groen) - bittene i CDJ'ens farve-
                           ;; waveform er roed/groen/blaa = bas/mellem/top (Deep Symmetrys
                           ;; egen protokolanalyse og pyrekordbox), men Color'en faar blaa
                           ;; paa groen-pladsen. Maalt paa 299 numre fra Tobias' bibliotek
                           ;; mod den sweep-verificerede preview (PWV4): groen-bittene
                           ;; foelger mellemtonen, blaa-bittene toppen i 298. Saa: mellem
                           ;; er .getBlue, top er .getGreen. Til og med runde 318 laeste
                           ;; vi dem omvendt, og "high" til QLC+ var mellemtonen (hats,
                           ;; fills, riser, phraseDark). Flagene aendrer sig ikke (92
                           ;; numre + 15 uden kick, analysis_eval) - derfor ingen version.
                           (let [c   (.segmentColor detail (int i) (int 1))
                                 rr  (double (.getRed c))
                                 gg  (double (.getBlue c))     ; mellem (se ovenfor)
                                 bb  (double (.getGreen c))    ; top
                                 sum (max 1.0 (+ rr gg bb))]
                             [h (* h (/ rr sum)) (* h (/ gg sum)) (* h (/ bb sum))])
                           ;; En blaa boelgeform har ingen baand. Det her er
                           ;; IKKE en rigtig opdeling - de tre tal er skalerede
                           ;; kopier af det samme, saa de er lineaert
                           ;; afhaengige og baandlogikken laenger nede kan
                           ;; stadig ikke bruges. Det holder dem bare vaek fra
                           ;; nul, saa qlc-normalize-to ikke dividerer paa et
                           ;; tomt baand. qlc-classify og qlc-unify-repeats
                           ;; slaar selv baandreglerne fra naar :mono er sat.
                           [h (* h 0.55) (* h 0.30) (* h 0.15)])))
        segs   (mapv band (range s0 s1))
        hs     (mapv #(nth % 0) segs)
        lws    (mapv #(nth % 1) segs)
        mws    (mapv #(nth % 2) segs)
        bws    (mapv #(nth % 3) segs)
        sumH   (reduce + hs)
        sumL   (reduce + lws)
        sumM   (reduce + mws)
        sumB   (reduce + bws)
        win    (fn [o] (qlc-mean-range lws o (+ o attack)))
        a      (max (win 0) (win (quot attack 2)) (win attack))
        r      (/ (- sumL (* a attack)) (max 1 (- n attack)))
        ;; where in the beat the bass peaks
        peak   (if (pos? sumL)
                 (/ (double (first (apply max-key second (map-indexed vector lws)))) n)
                 0.5)
        ;; transients in the mid/high bands: a rising snare roll shows up here
        mh     (mapv + mws bws)
        mean   (/ (+ sumM sumB) n)
        roll   (count (filter (fn [i] (and (> (mh i) (+ 1.0 (* 1.5 (mh (dec i)))))
                                          (> (mh i) (* 1.2 mean))))
                              (range 1 n)))
        ;; runde 358 (Tobias: "rytmen inde i slaget"): toppens anslag i hver
        ;; fjerdedel af beatet - hvor meget toppen stiger i fjerdedelens
        ;; foerste ottendedel over de tre rammer foer den. Klap og snare paa
        ;; 2 og 4, en off-beat hat i house, en dembows 3-3-2. Maalt paa 93
        ;; numre fra biblioteket: toppen paa 2/4 mindst 1,4 gange 1/3 i en
        ;; femtedel af dem, off-beat-hatten over beatet i en tredjedel.
        pre    (max 0 (- s0 4))
        ;; runde 415: never past the waveform's last frame (see hpk below)
        hi     (into (mapv #(nth (band %) 3) (range (min pre frames) (min s0 frames))) bws)
        np     (- s0 pre)
        qw     (max 2 (quot n 8))
        qon    (fn [q]
                 (let [a  (+ np (quot (* q n) 4))
                       e  (min (count hi) (+ a qw))
                       b0 (max 0 (- a 3))]
                   (if (>= a e)
                     0.0
                     (max 0.0 (- (reduce max (subvec hi a e))
                                 (if (> a b0) (qlc-mean-range hi b0 a) 0.0))))))
        ;; R461_ONSETS (Tobias 10-15, ide 2 og 3): bassens og mellemtonens anslag
        ;; pr. fjerdedel, som toppens ovenfor - bassen paa "og'et" i house, plucks og
        ;; percussion i breaks. Maalt paa 86 af hans numre (analysis_eval/lyd-analyse-1015).
        qon-of (fn [k q]
                 (let [v  (into (mapv #(nth (band %) k) (range (min pre frames) (min s0 frames)))
                                (if (= k 1) lws mws))
                       a  (+ np (quot (* q n) 4))
                       e  (min (count v) (+ a qw))
                       b0 (max 0 (- a 3))]
                   (if (>= a e)
                     0.0
                     (max 0.0 (- (reduce max (subvec v a e))
                                 (if (> a b0) (qlc-mean-range v b0 a) 0.0))))))
        ;; R493_A1_BLT_MEASURE (runde 493, swing og stabs): to ting mere fra de
        ;; samme rammer - intet nyt opslag i boelgeformen.
        ;; :sw - hvor toppens stigning (hatten) topper, som brok af beatet, i fire
        ;; vinduer: slaget, den foerste sekstendedel, ottendedelen og den sidste
        ;; sekstendedel. En parabel gennem de tre rammer om toppen giver brokdele
        ;; af en ramme. Kun en tydelig top (2,5 gange vinduets middel) - ellers nil.
        ;; Vinduerne er skaeve mod det sene: en shuffle flytter kun slag senere.
        flux   (fn [v i] (max 0.0 (- (double (nth v i)) (double (nth v (dec i))))))
        swpk   (fn [lo hf]
                 (let [a (max 1 (+ np (long (Math/floor (* lo n)))))
                       e (min (count hi) (+ np (long (Math/ceil (* hf n)))))]
                   (when (< (inc a) e)
                     (let [fs (mapv #(flux hi %) (range a e))
                           k  (apply max-key fs (range (count fs)))
                           f0 (fs k)
                           mn (/ (reduce + fs) (count fs))]
                       (when (and (pos? f0) (>= f0 (* 2.5 mn)))
                         (let [fl  (if (pos? k) (fs (dec k)) 0.0)
                               fr  (if (< (inc k) (count fs)) (fs (inc k)) 0.0)
                               den (- (+ fl fr) (* 2.0 f0))
                               off (if (neg? den) (max -0.5 (min 0.5 (/ (* 0.5 (- fl fr)) den))) 0.0)]
                           [(/ (+ (- (+ a k) np) off) (double n)) f0]))))))
        ;; :q-stab - en akkord-stab i mellemtonen pr. fjerdedel: [mellemtonens
        ;; stigning, hvor meget den falder igen inden fjerdedelen er omme (0-1: en
        ;; stab, ikke en flade der toner ind), toppens stigning]. qlc-curves vejer
        ;; de to stigninger mod hvert baands eget niveau (en stab over en off-beat-
        ;; hat er stadig en stab, en hat alene er ikke) - formen, ikke styrken.
        mi     (into (mapv #(nth (band %) 2) (range (min pre frames) (min s0 frames))) mws)
        ql     (max 2 (quot n 4))
        ;; R521_ROLL32_MEASURE (runde 521, Tobias 10-31: "lyset ruller med de hurtigste
        ;; trommehvirvler"): toppens og mellemtonens stigning paa den SKAEVE toogtredivtedel
        ;; i hver sekstendedel (1/8, 3/8, 5/8 og 7/8 af beatet), maalt som sekstendedelenes
        ;; anslag ovenfor: toppen i vinduet over middel af de tre rammer foer. Vinduet er tre
        ;; fjerdedele af en toogtredivtedel - et helt ville naa den naeste sekstendedels anslag
        ;; (sekstendedelens eget vindue, n/8, slutter hvor det her begynder). [top mellem].
        q32    (fn [k]
                 (let [a    (+ np (quot (* (inc (* 2 k)) n) 8))
                       w    (max 2 (quot (* 3 n) 32))
                       rise (fn [v]
                              (let [e  (min (count v) (+ a w))
                                    b0 (max 0 (- a 3))]
                                (if (>= a e)
                                  0.0
                                  (max 0.0 (- (reduce max (subvec v a e))
                                              (if (> a b0) (qlc-mean-range v b0 a) 0.0))))))]
                   [(rise hi) (rise mi)]))
        qstab  (fn [q]
                 (let [a  (+ np (quot (* q n) 4))
                       e  (min (count mi) (+ a qw))
                       b0 (max 0 (- a 3))
                       t0 (min (count mi) (+ a (quot (* 3 ql) 5)))
                       t1 (min (count mi) (+ a ql))]
                   (if (or (>= a e) (>= t0 t1))
                     [0.0 0.0 0.0]
                     (let [base (if (> a b0) (qlc-mean-range mi b0 a) 0.0)
                           pk   (reduce max (subvec mi a e))
                           rise (max 0.0 (- pk base))
                           top  (max 0.0 (- (reduce max (subvec hi a (min (count hi) e)))
                                            (if (> a b0) (qlc-mean-range hi b0 a) 0.0)))
                           fall (if (pos? rise)
                                  (max 0.0 (min 1.0 (/ (- pk (qlc-mean-range mi t0 t1)) rise)))
                                  0.0)]
                       [rise fall top]))))
        ;; runde 359 (Tobias: "pumpen foelger nummerets egen pumpen"): hvor
        ;; dybt lyden falder efter beatets anslag - 1 minus det stilleste
        ;; stykke (tre rammer) efter beatets foerste fjerdedel, delt med
        ;; anslaget. 86 numre: fra 0,60 (fyldige hele vejen) til 1,0 (intet
        ;; mellem kickene), median 0,93.
        q4     (max 1 (quot n 4))
        ;; runde 415 (aftenen 10-09, BLT's log): a beat past the end of the
        ;; waveform - the beat grid runs longer than the waveform detail on
        ;; some files - has no frames, hs is empty while n is 1, and this
        ;; subvec threw. qlc-send-track! failed on every try, and the track
        ;; NEVER reached QLC+ (7 numre den aften: 80-350 s uden lys). Since
        ;; runde 359 put the subvec here. Bounded by what is there.
        hpk    (reduce max 0.0 (subvec hs 0 (min (count hs) q4)))
        htr    (if (> (- n 3) q4)
                 (reduce min (map (fn [i] (/ (+ (hs i) (hs (inc i)) (hs (+ i 2))) 3.0))
                                  (range q4 (- n 3))))
                 hpk)
        pump   (if (pos? hpk) (max 0.0 (- 1.0 (/ htr hpk))) 0.0)]
    {:level (/ sumH (* 31.0 n))
     :low   (/ sumL (* 31.0 n))
     :mid   (/ sumM (* 31.0 n))
     :high  (/ sumB (* 31.0 n))
     :kick  (max 0.0 (/ (- a r) 31.0))
     :peak  peak
     :roll  roll
     :q-high (mapv qon (range 4))
     :q-low  (mapv #(qon-of 1 %) (range 4))     ; R461_ONSETS
     :q-mid  (mapv #(qon-of 2 %) (range 4))
     :sw     (mapv (fn [[lo hf]] (swpk lo hf))       ; R493_A1_BLT_KEYS: swing og stabs
                   [[-0.06 0.06] [0.19 0.40] [0.44 0.72] [0.69 0.90]])
     :q-stab (mapv qstab (range 4))
     :q-32   (mapv q32 (range 4))                   ; R521_ROLL32_KEY: de skaeve 32-dele
     :pump  pump}))

(defn qlc-grid-trust
  "Andel af kick-beats hvor bassen topper i attack-vinduet. Et skaevt beatgrid
   giver en lav andel, og saa maa vi finde fasen selv."
  [ms]
  (let [kicks (mapv :kick ms)
        thr   (qlc-percentile kicks 50)
        on    (filter #(> (:kick %) thr) ms)]
    (if (< (count on) 8)
      1.0
      (/ (double (count (filter #(or (< (:peak %) 0.22) (> (:peak %) 0.92)) on)))
         (count on)))))

(defn qlc-measure-all [detail grid total frames offset-ms]
  (mapv #(qlc-measure-beat detail grid % total frames offset-ms) (range 1 (inc total))))

(defn qlc-find-grid-offset
  "Proev 12 faser af et beat paa hvert 4. beat og vaelg den hvor kicket
   staar skarpest. Bruges kun naar grid'et ikke er til at stole paa."
  [detail grid total frames]
  (let [beat-ms (max 200.0 (/ (- (.getTimeWithinTrack grid (int (min total 33)))
                                 (.getTimeWithinTrack grid (int 1)))
                              (max 1 (dec (min total 33)))))
        sample  (range 1 (inc total) 4)
        score   (fn [off]
                  (let [ms (mapv #(qlc-measure-beat detail grid % total frames off) sample)]
                    (/ (reduce + (map :kick ms)) (max 1 (count ms)))))
        offs    (map #(long (* beat-ms (/ % 12.0))) (range 12))]
    (apply max-key score offs)))

;; R493_A1_BLT_SWING (runde 493, Tobias: swing chases): hvor sent sekstendedelene
;; og ottendedelen ligger - toppens stigninger (:sw fra qlc-measure-beat) mod
;; slagets EGEN stigning (midterste halvdels middel), saa boelgeformens og grid'ets forsinkelse
;; gaar ud. Kun tydelige hats taeller (se thr), og mindst 32 beats (eller en femtedel).
;; 1,0 = fuld triol-shuffle (sekstendedelen 1/12 beat sent, ottendedelen 1/6).
;; nil = for faa hats til at sige noget: QLC+ koerer lige, som i dag.
(defn qlc-swing [ms]
  (let [col    (fn [k] (vec (keep #(nth (:sw %) k nil) ms)))
        c0     (col 0)
        c1     (col 1)
        c2     (col 2)
        c3     (col 3)
        ;; a hat: 0,3 of the beat's and the eighth's typical rise, AND half the top's
        ;; own p90 per beat (the waveform's units - :high is divided by 31): without
        ;; the second a track with no hats measured its hiss (synthetic: 0,44)
        thr    (max (* 0.3 (qlc-percentile (mapv second (concat c0 c2)) 75))
                    (* 0.5 31.0 (qlc-percentile (mapv :high ms) 90)))
        need   (max 32 (quot (count ms) 5))
        strong (fn [ps] (filterv #(>= (second %) thr) ps))
        ;; the middle half's mean, not the median: a beat's frames are whole, and the
        ;; median of the beat's own rise sat on one frame or the next (half a frame off)
        med    (fn [xs] (when (>= (count xs) need)
                          (let [s (vec (sort xs)) c (count s)
                                m (subvec s (quot c 4) (max (inc (quot c 4)) (- c (quot c 4))))]
                            (/ (reduce + m) (count m)))))
        lat    (or (med (map first (strong c0))) 0.0)
        e8     (med (map #(- (first %) 0.5 lat) (strong c2)))
        e16    (med (concat (map #(- (first %) 0.25 lat) (strong c1))
                            (map #(- (first %) 0.75 lat) (strong c3))))
        unit   (fn [d full]
                 (when d (/ (Math/round (* 100.0 (max 0.0 (min 1.0 (/ d full))))) 100.0)))
        s8     (unit e8 (/ 1.0 6.0))
        s16    (unit e16 (/ 1.0 12.0))]
    {:swing   (when (or s8 s16) (max (or s8 0.0) (or s16 0.0)))
     :swing16 s16
     :swing8  s8}))

(defn qlc-curves [detail grid]
  (let [total  (.-beatCount grid)
        frames (.getFrameCount detail)
        ms0    (qlc-measure-all detail grid total frames 0)
        trust0 (qlc-grid-trust ms0)
        offset (if (< trust0 (qlc-opt :grid-trust-min 0.60))
                 (qlc-find-grid-offset detail grid total frames)
                 0)
        ms     (if (zero? offset) ms0 (qlc-measure-all detail grid total frames offset))
        trust  (if (zero? offset) trust0 (qlc-grid-trust ms))
        _      (when-not (.-isColor ^org.deepsymmetry.beatlink.data.WaveformDetail detail)
                 (try (timbre/warn "QLC+ boelgeformen er ikke farvet - baandene er gaettet, analysen er grovere")
                      (catch Throwable _ nil)))
        level  (mapv :level ms)
        low    (mapv :low ms)
        mid    (mapv :mid ms)
        high   (mapv :high ms)
        kick   (mapv :kick ms)
        roll   (mapv #(double (:roll %)) ms)
        ln     (qlc-normalize-to level (qlc-percentile level 95))
        ;; 75, ikke 90 som de andre. Bane B laeste det som en fejl (en
        ;; fjerdedel af alle beats klipper til 1,0), men det er med vilje:
        ;; wn bruges kun to steder, og begge kigger NEDAD. :energy er
        ;; (min ln wn), altsaa "loudness, men kun hvis der ogsaa er bas" -
        ;; en gate, ikke en maaling, og over 75-percentilen ER der bas nok,
        ;; saa loudness skal have lov at bestemme resten. house-break-reglen
        ;; nedenfor tester (< :low 0.35). Ingen af dem laeser toppen.
        ;; Flyttes den til 90 falder energien paa netop den fjerdedel hvor
        ;; drop'ene ligger, og :drop-min holder op med at ramme.
        wn     (qlc-normalize-to low   (qlc-percentile low 75))
        mn     (qlc-normalize-to mid   (qlc-percentile mid 90))
        hn     (qlc-normalize-to high  (qlc-percentile high 90))
        kn     (qlc-normalize-to kick  (qlc-percentile kick 90))
        rn     (qlc-normalize-to roll  (qlc-percentile roll 95))
        ;; runde 358: toppens anslag pr. fjerdedel, delt med nummerets egen
        ;; 90-percentil og pakket som fire nibbles pr. beat (0-15, 12 = p90)
        qall   (vec (mapcat :q-high ms))
        q90    (max 1.0e-6 (qlc-percentile qall 90))
        qpack  (mapv (fn [m]
                       (reduce + (map-indexed
                                   (fn [q x] (bit-shift-left
                                               (long (min 15 (Math/round (* 12.0 (/ (double x) q90)))))
                                               (* 4 q)))
                                   (:q-high m))))
                     ms)
        ;; R461_ONSETS: bassen og mellemtonen pakket som toppen (hver mod sin egen p90)
        pack   (fn [k]
                 (let [all (vec (mapcat k ms))
                       p90 (max 1.0e-6 (qlc-percentile all 90))]
                   (mapv (fn [m]
                           (reduce + (map-indexed
                                       (fn [q x] (bit-shift-left
                                                   (long (min 15 (Math/round (* 12.0 (/ (double x) p90)))))
                                                   (* 4 q)))
                                       (k m))))
                         ms)))
        ;; R493_A1_BLT_STAB: mellemtonens stabs. Hver hat smitter lidt af paa mellemtonen;
        ;; hvor meget, er nummerets eget forhold (lam: 25-percentilen af mellem/top-
        ;; stigning paa de tydelige top-anslag - ikke medianen: stabs paa hver anden
        ;; sekstendedel over hats er halvdelen af dem). En stab er det mellemtonen stiger UD OVER det -
        ;; mindst halvt op til mellemtonens p90 pr. beat (i boelgeformens enheder: :mid
        ;; og :high er delt med 31), ellers stoej - og den skal falde mindst halvvejs
        ;; tilbage inden fjerdedelen er omme (en flade der toner ind: 0,3). Gange faldet.
        ;; Pakket som anslagene (fire nibbles, 12 = p90 af de fjerdedele der HAR en
        ;; stab - de fleste har ingen). Under 16 i hele nummeret: nuller.
        spack  (let [mref (max 1.0e-6 (* 31.0 (qlc-percentile mid 90)))
                     href (max 1.0e-6 (* 31.0 (qlc-percentile high 90)))
                     rs   (vec (keep (fn [[rm _ rt]] (when (and rt (>= rt (* 0.5 href))) (/ rm rt)))
                                     (mapcat :q-stab ms)))
                     lam  (if (>= (count rs) 32) (qlc-percentile rs 25) 0.0)
                     sc   (fn [[rm fall rt]]
                            (let [x (if rm (- rm (* lam rt)) 0.0)]
                              (if (and (>= x (* 0.5 mref)) (>= fall 0.5)) (* x fall) 0.0)))
                     all  (filterv pos? (map sc (mapcat :q-stab ms)))
                     p90  (when (>= (count all) 16) (max 1.0e-6 (qlc-percentile all 90)))]
                 (mapv (fn [m]
                         (if p90
                           (reduce + (map-indexed
                                       (fn [q s] (let [x (sc s)]
                                                   (if (pos? x)
                                                     (bit-shift-left
                                                       (long (min 15 (Math/round (* 12.0 (/ x p90)))))
                                                       (* 4 q))
                                                     0)))
                                       (:q-stab m)))
                           0))
                       ms))
        ;; R521_ROLL32_PACK (runde 521): de skaeve toogtredivtedele pakket som anslagene - fire
        ;; nibbles pr. beat, 12 = SEKSTENDEDELENES egen p90 i samme baand, saa en hvirvel paa
        ;; 32-dele laeses med samme tal som en paa 16-dele. KUN hvor de er spillet: mindst 40 %
        ;; af p90 (QLC+'s hit-linje, 5) OG mindst halvt saa hoejt som sekstendedelens eget anslag
        ;; lige foer. En sekstendedels hale, en aaben hat der klinger ud, en shuffle (den sene
        ;; sekstendedel ligger foer vinduet) eller en enkelt flam giver nul. Den hoejeste af
        ;; toppen og mellemtonen (en lilletromme ligger i begge).
        p32    (let [m90 (max 1.0e-6 (qlc-percentile (vec (mapcat :q-mid ms)) 90))
                     nib (fn [x on p90]
                           (if (and (>= x (* 0.4 p90)) (>= x (* 0.5 on)))
                             (long (min 15 (Math/round (* 12.0 (/ (double x) p90)))))
                             0))]
                 (mapv (fn [m]
                         (reduce + (map-indexed
                                     (fn [k [t md]]
                                       (bit-shift-left
                                         (max (nib t (nth (:q-high m) k) q90)
                                              (nib md (nth (:q-mid m) k) m90))
                                         (* 4 k)))
                                     (:q-32 m))))
                       ms))
        sm     (qlc-opt :smooth-beats 4)]
    {:display     ln
     :energy      (qlc-smooth (mapv min ln wn) sm)
     :kick        (qlc-smooth kn sm)
     :low         (qlc-smooth wn sm)
     :mid         (qlc-smooth mn sm)
     :high        (qlc-smooth hn sm)
     :roll        (qlc-smooth rn sm)
     ;; kicket UGLATTET, slag for slag - analyse 10 laeser hvor kicket er i
     ;; hver takt, og glatningen over fem beats smoerer en takt uden kick ud
     :kick-raw    kn
     ;; runde 323: toppen uglattet ogsaa - et crash er ét slag, og glatningen
     ;; over fire beats goer det usynligt (QLC+'s "turn")
     :high-raw    hn
     ;; runde 358: mellemtonen uglattet (stemmen, melodien) og toppens anslag
     ;; pr. fjerdedel - QLC+ lader lyset foelge dem. Ikke fra en blaa boelgeform.
     :mid-raw     mn
     :onset-high  qpack
     ;; R461_ONSETS: bassens og mellemtonens anslag pr. fjerdedel
     :onset-low   (pack :q-low)
     :onset-mid   (pack :q-mid)
     ;; R493_A1_BLT_CURVES (runde 493): mellemtonens stabs og swingen - ikke fra
     ;; en blaa boelgeform (dens baand er gaettet)
     :onset-stab  spack
     ;; R521_ROLL32_CURVE (runde 521): de skaeve toogtredivtedeles anslag (0 = ikke spillet)
     :onset-32    p32
     :swing       (when (.-isColor ^org.deepsymmetry.beatlink.data.WaveformDetail detail)
                    (qlc-swing ms))
     ;; runde 359: hvor dybt lyden falder mellem kickene, slag for slag
     :pump-raw    (mapv :pump ms)
     ;; runde 317: hvad kurverne blev delt med - nummerets EGET kick og bas i
     ;; boelgeformens enheder. Kurverne er relative (et drop er fuldt i hvert
     ;; nummer), saa QLC+ bruger dem til at se hvor HAARDT kicket er
     ;; (strobernes chase over 75 %). Kun laest - intet skrives til CDJ'erne.
     :kick-ref    (qlc-percentile kick 90)
     :low-ref     (qlc-percentile low 75)
     :grid-trust  trust
     :grid-offset offset
     ;; en blaa boelgeform: baandene er gaettet, saa reglerne der bygger paa
     ;; forholdet mellem dem skal holde sig vaek
     :mono        (not (.-isColor ^org.deepsymmetry.beatlink.data.WaveformDetail detail))}))

;; ---------------------------------------------------------------- novelty (Foote)
;;
;; Bygger en selvsimilaritetsmatrix mellem takter paa [bas mellem top kick] og
;; koerer en skakbraet-kerne langs diagonalen. Toppe er sektionsgraenser -
;; uanset om overgangen er et trin eller en rampe.

(defn qlc-cosine [a b]
  (let [dot (reduce + (map * a b))
        na  (Math/sqrt (reduce + (map #(* % %) a)))
        nb  (Math/sqrt (reduce + (map #(* % %) b)))]
    (if (or (zero? na) (zero? nb)) 0.0 (/ dot (* na nb)))))

(defn qlc-bar-features
  "En vektor pr. takt: gennemsnit af bas, mellem, top og kick."
  [{:keys [low mid high kick]} downbeat]
  (let [n    (count low)
        bars (quot (- n downbeat) 4)]
    (mapv (fn [b]
            (let [a (+ downbeat (* 4 b)) e (+ a 4)]
              [(qlc-mean-range low a e) (qlc-mean-range mid a e)
               (qlc-mean-range high a e) (qlc-mean-range kick a e)]))
          (range bars))))

(defn qlc-novelty
  "Novelty pr. taktgraense b (mellem takt b-1 og b), kerne K takter til hver side."
  [feats K]
  (let [bars (count feats)
        sim  (fn [i j] (qlc-cosine (feats i) (feats j)))]
    (mapv (fn [b]
            (if (or (< b K) (> b (- bars K)))
              0.0
              (/ (reduce + (for [i (range K) j (range K)]
                             (+ (sim (- b 1 i) (- b 1 j))
                                (sim (+ b i) (+ b j))
                                (- (sim (- b 1 i) (+ b j)))
                                (- (sim (+ b i) (- b 1 j))))))
                 (* 4.0 K K))))
          (range (inc bars)))))

(defn qlc-novelty-beats
  "Novelty lagt ud paa beat-indeks: vaerdien paa taktlinjen, 0 imellem.
   Normaliseret til p95, saa den vejer som de andre signaler."
  [curves downbeat n]
  (let [feats (qlc-bar-features curves downbeat)
        nov   (qlc-novelty feats 4)
        ref   (qlc-percentile (filter pos? nov) 95)
        v     (vec (repeat n 0.0))]
    (reduce (fn [acc b]
              (let [i (+ downbeat (* 4 b))]
                (if (and (< i n) (> ref 1e-6))
                  (assoc acc i (min 1.0 (/ (nov b) ref)))
                  acc)))
            v
            (range (count nov)))))

(def ^:dynamic *qlc-novelty* nil)

;; ---------------------------------------------------------------- takter

(defn qlc-downbeat
  "0-indekseret index paa foerste beat der er 1-slaget i en takt."
  [grid total]
  (or (first (filter (fn [i] (= 1 (.getBeatWithinBar grid (int (inc i)))))
                     (range (min total 8))))
      0))

(defn qlc-transition
  "Overgangsstyrke ved i, paa 2, 4 og 8 takters skala - det stoerste taeller,
   saa baade skarpe trin og lange ramper fanges. Kick vaegter dobbelt. Dertil
   Foote-novelty paa taktlinjen, naar den er beregnet."
  [energy kick i]
  (+ (apply max (map (fn [w] (+ (* 2.0 (qlc-step kick i w)) (qlc-step energy i w)))
                     [8 16 32]))
     (if *qlc-novelty*
       (* (qlc-opt :novelty-weight 1.0) (get *qlc-novelty* i 0.0))
       0.0)))

(defn qlc-phrase-lines [n downbeat phase]
  (->> (range (+ downbeat (* 4 phase)) n 32)
       (filter #(and (>= % 16) (<= % (- n 16))))
       vec))

(defn qlc-phrase-phase
  "Hvilken af de 8 takter i frasen der bedst flugter med nummerets skift."
  [energy kick downbeat]
  (let [n (count energy)]
    (if (< n 64)
      0
      (apply max-key
             (fn [k] (reduce + (map #(qlc-transition energy kick %)
                                    (qlc-phrase-lines n downbeat k))))
             (range 8)))))

;; ---------------------------------------------------------------- sektioner

(def qlc-tier-name {0 "break" 1 "normal" 2 "drop"})

(defn qlc-section-type
  "Sektionens navn til markoeren. Som qlc-tier-name, men en groove der ligger
   HOEJT i nummerets eget spaend hedder \"drive\".

   Hvorfor: tier 1 daekkede baade et stille vers og en drivende passage, og
   lyset traekker fra samme programpulje i begge. Maalt paa Tobias' logs
   19-09 og 17-09 spaendte 'normal' 0.04-0.85 i sektionslyd - samme maerkat
   over hele registret. Time-line (time-line.io) deler netop dér: Drive =
   \"a steady, high-energy groove that maintains momentum without actively
   building or releasing tension\", over for Breakdown i den lave ende.
   Deres model er trænet paa EDM; det her er den samme skelnen maalt paa
   nummerets eget spaend, som break-reglen i qlc-classify allerede goer.

   Den ANDEN kilde til \"drive\" er et drop der ikke har fortjent navnet - se
  qlc-build-markers. Begge ender her.

  BAGUDKOMPATIBELT I BEGGE RETNINGER: en aeldre QLC+ kender ikke \"drive\"
   og falder tilbage til hverken break/build/drop = groove, altsaa praecis
   som foer. En nyere QLC+ uden \"drive\"-markoerer koerer ogsaa som foer."
  [seg]
  (if (and (= 1 (:tier seg)) (:drive seg)) "drive" (qlc-tier-name (:tier seg))))

(def ^:dynamic *qlc-curves* nil)

;; ------------------------------------------------- Pioneers egen graensedetektor
;;
;; AlphaTheta (Pioneer DJ) patent US10629173B2, "Musical piece development
;; analysis device". Det er den algoritme rekordbox' faseanalyse bygger paa, og
;; den er bevidst regelbaseret - patenten argumenterer imod mønstergenkendelse
;; fordi det skal kunne koere paa en DJ-laptop midt i et sæt.
;;
;; Ideen: del hver takt i 16 sekstendedele og noter hvor der er et kick. Ved
;; hver kandidat-takt sammenlignes de 8 takter FOER med de 8 takter EFTER,
;; takt for takt, og man taeller hvor mange af de 16 pladser der stemmer -
;; baade "kick begge steder" og "intet kick nogen af stederne" taeller med.
;; Det er dét sidste der gør at en breakdown, hvor kicket forsvinder helt,
;; giver et stort udslag.
;;
;; To detaljer fra patenten der er hele forskellen paa om det virker:
;;   * takt 4 og 8 springes over, fordi "der produceres generelt en masse
;;     uregelmaessige lyde (f.eks. fill-in paa trommer) i fjerde og ottende
;;     takt". Det er praecis derfor almindelige novelty-kurver fyrer fire
;;     takter for tidligt paa en riser.
;;   * i den foerste takt springes slag 1 over (angrebet efter et fill).
;; Max bliver dermed 5*16 + 12 = 92 sammenligninger.
;;
;; Q = M/92. Patenten bruger 0,90 som graense og tager laveste Q i hvert dyk.
;; Deres egne tal: 0,98-0,99 midt i en sektion, 0,89 praecis paa overgangen.
;;
;; Vi har ikke onsets, vi har et basbaand pr. half-frame - saa "er der et kick
;; i denne sekstendedel" bliver til "toppede bassen over en taerskel her".

(def qlc-patent-slots 16)          ; sekstendedele pr. takt, som i patenten
(def qlc-patent-max 92)            ; 5 takter a 16 pladser + 12 i den foerste

(defn qlc-low-frames
  "Basniveauet for hver half-frame i nummeret. Én gennemgang af boelgeformen,
   saa de 16 pladser pr. takt bagefter kun er opslag."
  [detail frames]
  (let [^org.deepsymmetry.beatlink.data.WaveformDetail detail detail
        col? (.-isColor detail)]
    (mapv (fn [i]
            (let [h (double (.segmentHeight detail (int i) (int 1)))]
              (if col?
                (let [c   (.segmentColor detail (int i) (int 1))
                      rr  (double (.getRed c))
                      gg  (double (.getGreen c))
                      bb  (double (.getBlue c))
                      sum (max 1.0 (+ rr gg bb))]
                  (* h (/ rr sum)))
                h)))
          (range frames))))

(defn qlc-slot-levels
  "Bassens top i hver sekstendedel, fra downbeat og frem. En vektor pr. takt."
  [lowf grid total frames offset-ms downbeat]
  (let [^org.deepsymmetry.beatlink.data.BeatGrid grid grid
        bar0  (inc downbeat)                       ; 1-indekseret beat
        bars  (quot (- (inc total) bar0) 4)
        at    (fn [b] (+ (.getTimeWithinTrack grid (int (min total (max 1 b)))) offset-ms))]
    (vec
      (for [bi (range (max 0 bars))]
        (vec
          (for [s (range qlc-patent-slots)]
            ;; bi! Uden den maalte hver eneste takt de samme fire slag,
            ;; alle taktvektorer blev ens, Q blev 1,0 overalt og detektoren
            ;; fandt aldrig noget som helst - uden at fejle.
            (let [b    (+ bar0 (* 4 bi) (quot s 4))
                  frac (/ (double (mod s 4)) 4.0)
                  ms0  (at b)
                  ms1  (at (inc b))
                  len  (max 1.0 (- ms1 ms0))
                  a    (+ ms0 (* len frac))
                  z    (+ a (/ len 4.0))
                  i0   (max 0 (long (* a qlc-half-frames-per-ms)))
                  i1   (min frames (max (inc i0) (long (* z qlc-half-frames-per-ms))))]
              (if (< i0 i1)
                (double (reduce max 0.0 (subvec lowf i0 i1)))
                0.0))))))))

(defn qlc-trim-silent-tail
  "Smid tomme takter i halen vaek. Naar boelgeformen daekker mindre end
   beatgrid'et lover, binariseres resten til nuller - og en stribe nuller
   ligner en breakdown, saa detektoren ville tvinge en falsk graense ind
   praecis der hvor dataene slap op."
  [levels]
  (let [live? (fn [bar] (pos? (reduce max 0.0 bar)))
        n     (count levels)
        last-live (or (first (filter #(live? (nth levels %)) (range (dec n) -1 -1))) -1)]
    (subvec (vec levels) 0 (inc last-live))))

(defn qlc-slot-grid
  "De 16 pladser pr. takt som 0/1. Taersklen er baade takt-relativ og global:
   uden det globale gulv ville en stille takt binariseres til stoej, og saa
   ville en breakdown ligne en almindelig takt."
  [levels]
  (let [flat (vec (sort (mapcat identity levels)))
        gp   (if (seq flat) (nth flat (min (dec (count flat))
                                           (long (* 0.75 (count flat))))) 0.0)
        floor (* 0.40 gp)]
    (mapv (fn [bar]
            (let [bmax (reduce max 0.0 bar)
                  thr  (max floor (* 0.50 bmax))]
              (mapv (fn [v] (if (and (pos? thr) (>= v thr)) 1 0)) bar)))
          levels)))

(defn qlc-conformity-at
  "Q for takt n: hvor meget de 8 takter foer ligner de 8 efter."
  [bars n]
  (let [cnt (count bars)]
    (when (and (>= n 8) (<= (+ n 7) cnt))    ; n+6 er det hoejeste vi roerer
      (let [m (reduce
                +
                (for [i [1 2 3 5 6 7]]                 ; takt 4 og 8 er fill-ins
                  (let [before (nth bars (+ (- n 9) i))
                        after  (nth bars (+ (- n 1) i))
                        from   (if (= i 1) 4 0)]       ; foerste takt: spring slag 1 over
                    (count (filter (fn [s] (= (nth before s) (nth after s)))
                                   (range from qlc-patent-slots))))))]
        (/ (double m) qlc-patent-max)))))

(defn qlc-conformity
  "Q-kurven for hele nummeret: {taktindex -> Q}. Foerste og sidste 8 takter
   kan ikke beregnes og er ikke med."
  [bars]
  (into {} (keep (fn [n] (when-let [q (qlc-conformity-at bars n)] [n q]))
                 (range (count bars)))))

(defn qlc-patent-dips
  "Takterne hvor Q dykker - laveste Q i hvert sammenhaengende dyk, som
   patenten foreskriver, med det dybeste foerst.

   To afvigelser fra patentens 0,90, begge afproevet paa syntetiske numre hvor
   graenserne var kendt paa forhaand:

   * Taersklen maales mod nummerets EGET loft i stedet for et fast tal. Dybt
     inde i en sektion er Q 1,0 paa rene data - patentens egne tal siger
     0,98-0,99 - men et nummer hvor bassen er rodet loefter hele kurven, og saa
     ville 0,90 fyre overalt. Vi tager 90-percentilen som \"her sker intet\"
     og traekker margenen fra den.
   * Ligger der en frasegraense (hver 8. takt) inde i dykket og er den naesten
     lige saa dyb, vinder den. Producere laegger skift paa 8-taktslinjer, og
     dykket smoerer sig gerne en takt til hver side.

   Paa rene testnumre: 16 af 18 graenser fundet, nul falske."
  [q margin]
  (let [ns (vec (sort (keys q)))]
    (when (>= (count ns) 8)
      (let [vals (vec (sort (map #(get q %) ns)))
            ceil (nth vals (min (dec (count vals)) (long (* 0.90 (count vals)))))
            thr  (- ceil margin)
            runs (->> ns
                      (filter #(< (get q %) thr))
                      (reduce (fn [acc n]
                                (if (and (seq acc) (= (inc (last (last acc))) n))
                                  (conj (vec (butlast acc)) (conj (last acc) n))
                                  (conj acc [n])))
                              []))]
        (->> runs
             (map (fn [run]
                    (let [best (apply min-key #(get q %) run)
                          line (filter #(and (zero? (mod % 8))
                                             (<= (get q %) (+ (get q best) 0.03)))
                                       run)]
                      (if (seq line) (apply min line) best))))
             distinct
             (sort-by #(get q %))
             vec)))))

(declare qlc-patent-cuts)

(defn qlc-patent-for
  "Pioneers graenser for et nummer, klar til at blive lagt i curves.

   Den skal kaldes af ALLE de steder der analyserer - baade det spor der
   spiller og dem der bliver klargjort i baggrunden. Ellers bliver et nummer
   der blev lagt paa den anden deck foerst analyseret uden patentgraenser,
   gemt i cachen, og saa er de graenser tabt for den aften."
  [detail grid curves]
  (let [total (.-beatCount grid)]
    (qlc-patent-cuts detail grid total (.getFrameCount detail)
                     (:grid-offset curves) (qlc-downbeat grid total))))

(defn qlc-patent-cuts
  "Patentens graenser som 0-indekserede beats, klar til at blive tvunget ind
   i qlc-best-cuts. Returnerer ogsaa kurven, saa den kan logges.

   Hvis noget som helst herinde fejler - en boelgeform vi ikke kan laese, et
   nummer der er for kort - falder vi tilbage paa vores egen analyse alene.
   Det her maa kun goere resultatet bedre, aldrig daarligere."
  [detail grid total frames offset-ms downbeat]
  (try
    (when (and (qlc-opt :patent-cuts true) (>= total 96))
      (let [lowf   (qlc-low-frames detail frames)
            levels (qlc-trim-silent-tail
                     (qlc-slot-levels lowf grid total frames offset-ms downbeat))
            bars   (qlc-slot-grid levels)
            q      (qlc-conformity bars)
            dips   (or (qlc-patent-dips q (qlc-opt :patent-margin 0.10)) [])
            ;; Konservativt: de dybeste dyk foerst, og aldrig flere end vi
            ;; sigter efter i forvejen. De her graenser bliver TVUNGET ind, saa
            ;; en falsk positiv koster mere end en overset - vores egen
            ;; energianalyse finder alligevel de fleste af dem selv.
            keepn  (qlc-target-sections total)   ; den bunder selv ved 3
            picked (take keepn dips)]
        {:cuts  (vec (sort (map #(+ downbeat (* 4 %)) picked)))
         :curve q
         :bars  (count bars)
         :found (count dips)}))
    (catch Throwable e
      (try (timbre/warn "QLC+ patent-graenser sprang fra:" (.getMessage e))
           (catch Throwable _ nil))
      nil)))

(defn qlc-seg [energy kick a b]
  (let [c *qlc-curves*]
    {:from   (inc a)
     :to     b
     :energy (qlc-mean-range energy a b)
     :kick   (qlc-mean-range kick a b)
     :low    (if c (qlc-mean-range (:low c) a b) 0.0)
     :mid    (if c (qlc-mean-range (:mid c) a b) 0.0)
     :high   (if c (qlc-mean-range (:high c) a b) 0.0)
     :roll   (if c (qlc-mean-range (:roll c) a b) 0.0)}))

(defn qlc-segs-from-cuts [energy kick cuts]
  (mapv (fn [[a b]] (qlc-seg energy kick a b))
        (partition 2 1 (concat [0] cuts [(count energy)]))))

(defn qlc-absorb-short
  "Opsug sektioner kortere end minlen i den nabo de ligner mest. Foerste og
   sidste sektion maa vaere kortere - intro og outro er ofte 4 takter."
  [segs minlen energy kick]
  (loop [ss (vec segs) guard 0]
    (if (or (> guard 300) (<= (count ss) 1))
      ss
      (let [edge (* 4 (qlc-opt :edge-min-bars 4))
            last (dec (count ss))
            i    (first (keep-indexed
                          (fn [i s]
                            (let [len (inc (- (:to s) (:from s)))
                                  lim (if (or (zero? i) (= i last)) (min minlen edge) minlen)]
                              (when (< len lim) i)))
                          ss))]
        (if (nil? i)
          ss
          (let [dist (fn [x y] (+ (Math/abs (- (:kick x) (:kick y)))
                                  (Math/abs (- (:energy x) (:energy y)))))
                ;; runde 210: the neighbour the merge keeps within the
                ;; longest section - absorbing into the most similar one grew
                ;; sections past 32 bars (up to ~44), one look held that long
                mlen (fn [j] (inc (- (:to (nth ss (max i j))) (:from (nth ss (min i j))))))
                tgt  (cond
                       (zero? i) 1
                       (= i (dec (count ss))) (dec i)
                       :else (let [pick  (if (< (dist (nth ss (dec i)) (nth ss i))
                                                (dist (nth ss (inc i)) (nth ss i)))
                                           (dec i) (inc i))
                                   other (if (= pick (dec i)) (inc i) (dec i))]
                               (if (and (> (mlen pick) (qlc-max-beats)) (<= (mlen other) (qlc-max-beats)))
                                 other pick)))
                lo   (min i tgt)
                hi   (max i tgt)
                m    (qlc-seg energy kick (dec (:from (nth ss lo))) (:to (nth ss hi)))]
            (recur (vec (concat (subvec ss 0 lo) [m] (subvec ss (inc hi))))
                   (inc guard))))))))

(defn qlc-target-sections [beats]
  (let [bars (quot beats 4)]
    (max 3 (min 12 (quot bars (max 1 (qlc-opt :bars-per-section 24)))))))

(defn qlc-cue-beats
  "0-indekserede beats for nummerets hot cues og memory cues. DJ'en har sat
   dem paa frasegraenser - det er det bedste bevis der findes."
  [grid player]
  (try
    (let [mf   (org.deepsymmetry.beatlink.data.MetadataFinder/getInstance)
          md   (.getLatestMetadataFor mf (int player))
          cues (when md (.getCueList md))]
      (if cues
        (->> (.-entries cues)
             (remove #(.-isLoop %))
             (map #(.findBeatAtTime grid (long (.-cueTime %))))
             (filter pos?)
             (map dec)
             distinct
             vec)
        []))
    (catch Throwable _ [])))

(defn qlc-snap-bar
  "Naermeste taktlinje til i."
  [i downbeat]
  (+ downbeat (* 4 (Math/round (/ (- i downbeat) 4.0)))))

(defn qlc-best-cuts
  "Vaelg den taerskel der giver et fornuftigt antal sektioner for laengden.
   extra er graenser der altid skal med (cue points)."
  [energy kick lines extra]
  (let [scores (mapv (fn [i] {:idx i :score (qlc-transition energy kick i)}) lines)
        ref    (qlc-percentile (mapv :score scores) 90)
        target (qlc-target-sections (count energy))
        fs     (qlc-opt :force-step 0.35)
        n      (count energy)
        forced (into (set (filter #(> (qlc-step energy % 8) fs) lines))
                     (filter #(and (>= % 16) (<= % (- n 16))) extra))
        tries  (mapv (fn [r]
                       (let [cuts (->> (concat (map :idx (filter #(> (:score %) (* ref r)) scores))
                                               forced)
                                       distinct
                                       sort
                                       vec)
                             segs (qlc-absorb-short (qlc-segs-from-cuts energy kick cuts)
                                                    (qlc-min-beats) energy kick)]
                         {:ratio r :cuts cuts :n (count segs)}))
                     [0.20 0.28 0.36 0.45 0.55 0.68 0.82])]
    (apply min-key
           (fn [t] (+ (Math/abs (- (:n t) target)) (* 0.01 (:ratio t))))
           tries)))

(defn qlc-split-long
  "Ingen sektion maa vaere laengere end max-beats. Laengere deles ved den
   staerkeste overgang paa en frasegraense inde i den - det er det der sikrer
   at en lang rampe eller et sent drop ikke bliver slugt af en enkelt sektion."
  [cuts energy kick lines]
  (let [n      (count energy)
        maxlen (qlc-max-beats)]
    (loop [cuts (vec (sort (distinct cuts))) guard 0]
      (let [new (for [[a b] (partition 2 1 (concat [0] cuts [n]))
                      :when (> (- b a) maxlen)
                      :let  [inside (filter #(and (>= % (+ a 16)) (<= % (- b 16))) lines)]
                      :when (seq inside)]
                  (apply max-key #(qlc-transition energy kick %) inside))]
        (if (or (empty? new) (> guard 20))
          cuts
          (recur (vec (sort (distinct (concat cuts new)))) (inc guard)))))))

(defn qlc-refine-cuts
  "Flyt hver graense til den taktlinje inden for +-refine-bars hvor kicket
   skifter mest - hvis skiftet der er klart staerkere end paa frasegraensen."
  [cuts kick]
  (let [n    (count kick)
        rb   (qlc-opt :refine-bars 2)
        gain (qlc-opt :refine-gain 1.25)
        sc   (fn [i] (qlc-step kick i 4))]
    (->> cuts
         (mapv (fn [c]
                 (let [cands (->> (range (- rb) (inc rb))
                                  (map #(+ c (* 4 %)))
                                  (filter #(and (>= % 8) (<= % (- n 8)))))
                       best  (if (seq cands) (apply max-key sc cands) c)]
                   (if (> (sc best) (* gain (sc c))) best c))))
         distinct
         sort
         vec)))

(defn qlc-otsu
  "Taerskel der bedst deler vaerdierne i to klasser (Otsu, 32 bins). Kick er
   bimodalt - kick paa / kick af - saa det er den rigtige taerskel for netop
   dette nummer, uden tuning."
  [values]
  (let [bins 32
        v    (vec values)
        n    (count v)
        hist (reduce (fn [h x] (update h (min (dec bins) (int (* (max 0.0 x) bins))) inc))
                     (vec (repeat bins 0)) v)
        tot  (reduce + (map-indexed (fn [i c] (* i c)) hist))]
    (loop [t 0, wb 0, sb 0, best -1.0, thr 0.5]
      (if (>= t bins)
        thr
        (let [wb' (+ wb (hist t))
              sb' (+ sb (* t (hist t)))
              wf  (- n wb')]
          (if (or (zero? wb') (zero? wf))
            (recur (inc t) wb' sb' best thr)
            (let [mb  (/ sb' (double wb'))
                  mf  (/ (- tot sb') (double wf))
                  var (* wb' wf (Math/pow (- mb mf) 2))]
              (if (> var best)
                (recur (inc t) wb' sb' var (/ (+ t 1.0) bins))
                (recur (inc t) wb' sb' best thr)))))))))

(defn qlc-kick-threshold
  "Otsu paa kick-kurven, hvis den lander et fornuftigt sted; ellers :kick-on."
  [kick]
  (let [t (qlc-otsu kick)]
    (if (and (>= t 0.20) (<= t 0.70)) t (qlc-opt :kick-on 0.40))))

(defn qlc-classify
  "Kick fra -> break. Kick tilbage efter break MED hoej energi -> drop.
   Ellers groove, medmindre energien loefter sig til fuld - saa er det drop.

   'Hoej' er relativt til nummeret: energien er normaliseret til p95, saa
   enhver sektion med kick ligger paa 0.75-1.0, og :drop-min 0.60 alene gjorde
   den FOERSTE sektion efter et break til drop hver gang - ogsaa en riser med
   pad og stigende niveau uden kick, som :loud-on lod slippe igennem som
   'kick paa'. Det var kilden til 65 % drop / 1 % normal i loggen fra 05-09.
   Nu skal kicket faktisk vaere tilbage, og energien skal naa :drop-rel af
   nummerets hoejeste sektion."
  [segs kick]
  (let [aq   (qlc-opt :abs-quiet 0.25)
        kt   (qlc-kick-threshold kick)
        lt   (qlc-opt :loud-on 0.65)
        dm   (qlc-opt :drop-min 0.60)
        af   (qlc-opt :abs-full 0.85)
        rise (qlc-opt :drop-rise 0.20)
        es   (mapv :energy segs)
        emax (reduce max 0.0 es)
        emin (reduce min 1.0 es)
        ;; ---------------------------------------------------------------
        ;; Et break er stille FOR DET HER NUMMER, ikke stille i absolut
        ;; forstand. Tobias, 2026-09-17, med to skaermbilleder: alt stod som
        ;; NORMAL. Regnet efter paa hans egne tal spaendte det ene nummer
        ;; 34-81 % og det andet 42-76 % - og :abs-quiet 0.25 og :abs-full
        ;; 0.85 ligger BEGGE uden for de intervaller. Med faste taersklers
        ;; gulv under og loft over dataene kan intet nummer blive andet end
        ;; normal, uanset hvor tydelig strukturen er for oeret.
        ;;
        ;; Derfor: den laveste tredjedel af nummerets eget spaend er et
        ;; break. Og saa retter drop-detektionen sig selv, for et drop er i
        ;; forvejen defineret som "kicket tilbage efter et break" - der var
        ;; bare aldrig et break at komme tilbage fra.
        ;;
        ;; Spaerren mod den modsatte groeft (65 % drop / 1 % normal, loggen
        ;; fra 05-09): er spaendet under :rel-spread, ER nummeret ensartet,
        ;; og saa bliver det normalt hele vejen som foer.
        spread (- emax emin)
        wide?  (>= spread (qlc-opt :rel-spread 0.12))
        rlo    (+ emin (* (qlc-opt :rel-quiet 0.33) spread))
        dm   (max dm (* (qlc-opt :drop-rel 0.90) emax))]
    (reduce (fn [acc s]
              (let [prev (peek acc)
                    e    (:energy s)
                    on?  (or (> (:kick s) kt) (> e lt))
                    ;; et drop kraever kicket - loudness alene er en riser
                    kick? (> (:kick s) kt)
                    ;; the house break: the kick leaves, the hats stay, the pad
                    ;; comes up - bass down while the top band is still there
                    ;; ikke paa en blaa boelgeform: der er high altid <= low
                    ;; efter normaliseringen, saa reglen er aritmetisk umulig
                    ;; og ville bare give falsk tryghed
                    hats (and (not (:mono *qlc-curves*))
                              (< (:low s) 0.35) (> (:high s) 0.45) (< e 0.75))
                    tier (cond
                           (< e aq)                                        0
                           ;; stille for DETTE nummer - se noten ovenfor
                           (and wide? (<= e rlo))                          0
                           (not on?)                                       0
                           hats                                            0
                           (and prev (= 0 (:tier prev)) kick? (>= e dm))  2
                           ;; ogsaa her kraever et drop kicket (runde 194) -
                           ;; en hoej riser uden kick blev et drop
                           (and prev kick? (>= e af) (>= (- e (:energy prev)) rise)) 2
                           :else                                           1)
                    ;; DRIVE: en groove i den oeverste del af nummerets eget
                    ;; spaend. Samme relative maalestok som break-reglen
                    ;; ovenfor - og kun paa et nummer der HAR struktur
                    ;; (wide?), for ellers er alting lige hoejt og ordet
                    ;; siger intet. Maalt paa logsne: 61-65 % af
                    ;; groove-sektionerne. Se qlc-section-type.
                    rdr   (+ emin (* (qlc-opt :drive-rel 0.70) spread))
                    drive (and wide? (= 1 tier) (>= e rdr) kick?)]
                (conj acc (assoc s :tier tier :drive (boolean drive)))))
            []
            segs)))

(defn qlc-unify-repeats
  "Drop 2 lyder som drop 1: sektioner der ligner hinanden (bas/mellem/top/kick)
   faar samme tier - den hoejeste af de to. Kun blandt sektioner med kick.

   Cosinus mellem fire positive tal fra det samme nummer er naesten altid over
   0.97 (groove mod drop: 0.996), saa energiafstanden er det eneste der holder
   igen - og 0.12 var saa bredt at naesten hver groove blev loeftet til drop
   saa snart nummeret havde ét. :repeat-energy strammer det. Paa en blaa
   (mono) boelgeform er baandene skalerede kopier af hinanden, cosinus er 1.0
   og reglen siger intet: der springes over, som kommentaren i qlc-measure
   lovede."
  [segs]
  (let [thr  (qlc-opt :repeat-sim 0.97)
        de   (qlc-opt :repeat-energy 0.05)
        ;; runde 210: a drop needs the kick HERE too (the runde-194 rule in
        ;; qlc-classify) - a section that is tier 1 on loudness alone was
        ;; lifted to a drop by a drop it resembled
        kt   (if-let [k (seq (:kick *qlc-curves*))] (qlc-kick-threshold (vec k)) 0.0)
        feat (fn [s] [(:low s) (:mid s) (:high s) (:kick s)])
        v    (vec segs)
        n    (count v)]
    (loop [i 0, v v]
      (if (or (>= i n) (:mono *qlc-curves*))
        v
        (recur (inc i)
               (reduce (fn [acc j]
                         (let [a (acc i) b (acc j)]
                           (if (and (pos? (:tier a)) (pos? (:tier b))
                                    (> (qlc-cosine (feat a) (feat b)) thr)
                                    (< (Math/abs (- (:energy a) (:energy b))) de))
                             (let [t (max (:tier a) (:tier b))]
                               (if (and (= t 2) (not (and (> (:kick a) kt) (> (:kick b) kt))))
                                 acc
                                 (-> acc (assoc-in [i :tier] t) (assoc-in [j :tier] t))))
                             acc)))
                       v
                       (range (inc i) n)))))))

(defn qlc-coalesce
  "Slaa nabo-sektioner med samme tier sammen - men ikke hen over et stort
   energispring, for det er information lyset skal have - og aldrig laengere
   end max-section-bars: qlc-split-long koerer FOER klassificeringen, saa uden
   loftet her voksede et nummer uden break tilbage til én sektion paa 700 beats
   (set i loggen fra 07-09), og lyset stod stille i fem minutter."
  [segs]
  (let [maxlen (qlc-max-beats)]
  (reduce (fn [acc s]
            (let [p (peek acc)]
              (if (and p
                       (= (:tier p) (:tier s))
                       ;; en drive og en normal er samme tier, men ikke det
                       ;; samme - de slaas ikke sammen (runde 194)
                       ;; kun i tier 1: qlc-unify-repeats loefter en drive til
                       ;; tier 2 uden at rydde :drive (runde 195)
                       (or (not= 1 (:tier s)) (= (boolean (:drive p)) (boolean (:drive s))))
                       (< (Math/abs (- (:energy p) (:energy s))) 0.25)
                       (<= (inc (- (:to s) (:from p))) maxlen))
                (conj (pop acc)
                      (assoc p :to (:to s)
                               :energy (max (:energy p) (:energy s))
                               :kick   (max (:kick p) (:kick s))))
                (conj acc s))))
          []
          segs)))

(defn qlc-analyse
  "Hele pipelinen. cues er 0-indekserede beats fra rekordbox (kan vaere tom)."
  ([curves grid] (qlc-analyse curves grid []))
  ([{:keys [energy kick] :as curves} grid cues]
   (let [n        (count energy)
         downbeat (qlc-downbeat grid n)]
     (binding [*qlc-curves*  curves
               *qlc-novelty* (qlc-novelty-beats curves downbeat n)]
       (let [phase    (qlc-phrase-phase energy kick downbeat)
             lines    (qlc-phrase-lines n downbeat phase)
             cuebars  (mapv #(qlc-snap-bar % downbeat) cues)
             ;; Pioneers graenser taeller som cue points: de er taktjusterede
             ;; i forvejen, og de kommer fra en helt anden slags maaling end
             ;; vores energikurve - et kickmoenster, ikke et niveau. Dukker de
             ;; ikke op (kort nummer, ulaeselig boelgeform), koerer vi videre
             ;; paa vores egen analyse alene.
             patent   (vec (:cuts (:patent curves)))
             forcebar (vec (distinct (concat cuebars patent)))
             best     (qlc-best-cuts energy kick lines forcebar)
             cuts     (-> (:cuts best)
                          (qlc-split-long energy kick lines)
                          (qlc-refine-cuts kick))
             segs     (-> (qlc-segs-from-cuts energy kick cuts)
                          (qlc-absorb-short (qlc-min-beats) energy kick)
                          (qlc-classify kick)
                          qlc-unify-repeats
                          qlc-coalesce)]
         {:sections segs :phase phase :downbeat downbeat :cues (count cuebars)
          :patent (count patent) :patent-found (:found (:patent curves))
          :kick-threshold (qlc-kick-threshold kick)
          :grid-trust (:grid-trust curves) :grid-offset (:grid-offset curves)
          :ratio (:ratio best) :target (qlc-target-sections n) :got (:n best)})))))

(defn qlc-build-start
  "0-indekseret start paa build'et foer et drop ved index d: gaa baglaens i
   takter saa laenge energien stiger, mellem build-bars og build-max-bars,
   men aldrig foer lo (start paa den foregaaende sektion).

   nil naar der ikke er noget build. Foer returnerede den (max s lo) uanset
   hvad, saa selv en groove der laa fuldstaendig fladt lige ind i drop'et fik
   et build-flag paa - og lyset kvitterede med en riser der ikke fandtes.

   Selve gaten maaler paa HELE sektionen: sidste takt foer drop'et skal ligge
   :build-rise over sektionens FOERSTE takt. To andre maader var forkerte og
   blev proevet foerst:

   - `rising?` paa ét 8-beats vindue ved build'ets start. En riser der stiger
     jaevnt over 100 beats stiger kun ~0,028 paa otte af dem, altsaa under
     gain, og en helt tydelig riser blev afvist.
   - sidste takt mod takten lige foer build-starten. Den almindelige EDM-form
     - riseren topper 30-40 beats foer drop'et og HOLDER derinde - har begge
     vinduer oppe paa plateauet, saa forskellen er nul og build'et forsvandt.

   Sektionens foerste takt er bunden riseren rejste sig fra, og den ligger
   fast uanset hvor i sektionen stigningen sker. Afproevet paa otte former:
   flad, jaevn riser, stejl riser, naesten flad, faldende, riser+plateau,
   break-saa-riser og stoej uden trend - alle otte svarer rigtigt."
  [energy d lo]
  (let [minb (qlc-build-beats)
        maxb (* 4 (qlc-opt :build-max-bars 32))
        gain (qlc-opt :build-rise 0.03)
        rising? (fn [s] (< (+ (qlc-mean-range energy (- s 8) s) gain)
                           (qlc-mean-range energy s (+ s 8))))
        ;; to baner vaek fra kanterne (runde 194): energien er glattet over
        ;; 5 beats centreret, saa de sidste 8 beats foer drop'et havde 7,5 %
        ;; af drop'ets spring i sig - en helt flad sektion 0,3 -> drop 0,8
        ;; maalte 0,0375 og fik et build. Samme halve vindue fra starten.
        floor   (qlc-mean-range energy (+ lo 2) (+ lo 10))
        top     (qlc-mean-range energy (- d 10) (- d 2))]
    (when (>= top (+ floor gain))
      (loop [s (- d minb)]
        (if (and (> (- s 4) lo) (< (- d s) maxb) (rising? s))
          (recur (- s 4))
          (max s lo))))))

(defn qlc-roll-start
  "Start paa snare-rollen foer et drop ved index d: gaa baglaens takt for takt
   saa laenge roll-taetheden falder bagud (dvs. stiger mod drop'et). Kraever at
   rollen er vokset mindst :roll-gain. nil hvis der ikke er nogen roll."
  [roll d lo]
  ;; taktvinduerne ligger to beats foer drop'et (runde 194): rollen er
  ;; glattet over 5 beats, og takt 1 = [d-4, d) havde 15 % af drop'ets
  ;; niveau i sig - en flad roll fik et build over hele sektionen
  (let [bar  (fn [k] (qlc-mean-range roll (- d (* 4 k) 2) (- d (* 4 (dec k)) 2)))
        gain (qlc-opt :roll-gain 1.30)
        maxk (min 16 (quot (- d lo) 4))]
    (when (>= maxk 2)
      (let [k (loop [k 1]
                (if (and (< k maxk) (<= (bar (inc k)) (* 1.05 (bar k))))
                  (recur (inc k))
                  k))]
        (when (and (>= k 2) (> (bar 1) (* gain (max 0.02 (bar k)))))
          (- d (* 4 k)))))))

(defn qlc-build-markers
  "Sektionsmarkoerer, plus build foran hvert drop. Build'et starter hvor
   snare-rollen begynder at stige, hvis der er en - ellers hvor energien gaar op."
  ([curves grid] (qlc-build-markers curves grid []))
  ([curves grid cues]
  (let [energy (:energy curves)
        roll   (:roll curves)
        runs   (:sections (qlc-analyse curves grid cues))
        n      (count runs)
        ;; -------------------------------------------------------------
        ;; ET DROP SKAL HAVE FORTJENT NAVNET.
        ;;
        ;; qlc-classify kalder et drop hver gang kicket kommer tilbage
        ;; efter et break med hoej nok energi. I house sker det 4-8 gange
        ;; pr. nummer, og maalt paa Tobias' cache (342 numre, 20-09) stod
        ;; der **kun et break foran 54 % af alle drops** - ingen opbygning
        ;; overhovedet. 72 % af numrene havde flere drop-markoerer end
        ;; groove-markoerer, og "MAXI MERAKI - Me and You" fik otte drops
        ;; og nul grooves. Det er ikke otte drops, det er én drivende
        ;; passage med huller i.
        ;;
        ;; Time-lines to definitioner skiller praecis dér: Drop = "the
        ;; primary high-energy section where musical tension is RELEASED";
        ;; Drive = "a steady, high-energy groove that maintains momentum
        ;; WITHOUT actively building or releasing tension". Udloesning
        ;; kraever opbygning.
        ;;
        ;; Saa: et drop beholder navnet hvis der enten er maalt en riser
        ;; foran det (samme maaling som build-markoeren bruger), eller hvis
        ;; energien springer :drop-lift op fra sektionen foer. Ellers er det
        ;; en drive. Maalt paa cachen: 996 drops -> 525 drops + 471 drives,
        ;; median 2 drops pr. nummer, og kun 11 % af numrene uden et eneste
        ;; drop. Uden loeft-undtagelsen blev det 255 drops og 33 % af
        ;; numrene helt uden - for haardt.
        built  (set (keep (fn [[i r]]
                            (let [nxt (when (< (inc i) n) (nth runs (inc i)))]
                              (when (and nxt (= 2 (:tier nxt))
                                         (or (qlc-roll-start roll (dec (:from nxt)) (dec (:from r)))
                                             (qlc-build-start energy (dec (:from nxt)) (dec (:from r)))))
                                (inc i))))
                          (map-indexed vector runs)))
        lift   (qlc-opt :drop-lift 0.35)
        earned? (fn [i r]
                  (or (contains? built i)
                      (let [prev (when (pos? i) (nth runs (dec i)))]
                        (and prev (>= (- (:energy r) (:energy prev)) lift)))))
        ;; ... og saa er navnet ét sted: en ufortjent drop-sektion faar
        ;; tier 1 + :drive, hvilket qlc-section-type oversaetter til
        ;; "drive". runs selv roeres ikke, saa build-indsaetningen nedenfor
        ;; regner paa de samme tiers som foer.
        label  (fn [i r]
                 (qlc-section-type (if (and (= 2 (:tier r)) (not (earned? i r)))
                                     (assoc r :tier 1 :drive true)
                                     r)))]
    (->> (map-indexed vector runs)
         (mapcat
           (fn [[i r]]
             (let [nxt   (when (< (inc i) n) (nth runs (inc i)))
                   drop? (and nxt (= 2 (:tier nxt)))
                   e     (:energy r)
                   lo    (dec (:from r))
                   bs    (when drop?
                           (or (qlc-roll-start roll (dec (:from nxt)) lo)
                               (qlc-build-start energy (dec (:from nxt)) lo)))
                   ;; build? = der ER maalt en riser. Uden den bliver drop'et
                   ;; bare et drop, og sektionen foran beholder sin egen type.
                   drop? (and drop? (some? bs))
                   ;; et build 4-12 beats efter sektionsflaget slettes af
                   ;; QLC+' egen oprydning (under fire takter) - det behandles
                   ;; som et build der raekker helt tilbage (runde 194)
                   whole? (and drop? (or (<= bs lo) (< (- (inc bs) (:from r)) 16)))
                   ;; build-flaget bar sektionens energi - altsaa energien FOER
                   ;; riseren. Lyset laeste derfor en riser som "her sker der
                   ;; lige saa lidt som i groove'en" hele vejen op. Den skal
                   ;; baere sin egen: middel over de beats build'et daekker.
                   be    (when drop?
                           (qlc-mean-range energy (max 0 bs) (dec (:from nxt))))]
               (cond
                 ;; BEGGE. Naar build'et raekker helt tilbage til sektionens
                 ;; start blev sektionens egen type kastet vaek - saa en
                 ;; breakdown foer et drop forsvandt helt, og lyset holdt
                 ;; drop-billedet lige ind i riseren. 46 af 188 numre i
                 ;; cachen ser saadan ud.
                 ;; En hel takt inde, ikke ét beat: QLC+ snapper markoerer til
                 ;; naermeste taktlinje, saa from og from+1 landede paa SAMME
                 ;; beat - og saa slettede QLC+' egen oprydning den ene, uden
                 ;; at det ligger fast hvilken. 16 beats er nok til at begge
                 ;; overlever, og kun naar sektionen har plads til det.
                 (and whole? (>= (- (:to r) (:from r)) 31))
                 [{:beat (:from r) :type (label i r) :energy e}
                  {:beat (+ (:from r) 16) :type "build" :energy be}]

                 whole?
                 [{:beat (:from r) :type "build" :energy be}]

                 drop?
                 [{:beat (:from r) :type (label i r) :energy e}
                  {:beat (inc bs) :type "build" :energy be}]

                 :else
                 [{:beat (:from r) :type (label i r) :energy e}]))))
         (sort-by :beat)
         (partition-by :beat)
         (map last)
         vec))))

;; ------------------------------------------------- rekordbox' egen faseanalyse
;;
;; Rekordbox gemmer sin Phrase-analyse i en PSSI-tag i .EXT-filen, og fra
;; rekordbox 6 bliver den eksporteret til USB'en, saa CDJ'en kan styre lys med
;; den. Naar den findes, er den bedre end vores egen: det er den samme
;; opdeling DJ'en ser paa displayet. Tobias, 2026-09-22: "Rekordbox
;; frase-analyse skal kun override vores, hvis den findes, og det skal laves
;; saa det virker."
;;
;; Reglen: HAANDRETTEDE flag > rekordbox' faser > vores egen analyse.
;;
;; Formatet (crate-digger, rekordbox_anlz.ksy - slaaet op 2026-09-22):
;;   TaggedSection .body -> SongStructureTag .body -> SongStructureBody
;;   (den ekstra .body er XOR-afmaskningen fra rekordbox 6; aeldre crate-digger
;;   havde mood/entries direkte paa taggen - begge dele virker her)
;;   .mood     -> enum TrackMood (1 high, 2 mid, 3 low)       -> .id -> long
;;   .entries  -> SongStructureEntry: .beat (u2, 1-baseret), .kind -> en
;;                PhraseHigh/Mid/Low-struct -> .id -> enum -> .id -> long
;;   mood 1 "high"  -> Intro / Up / Down / Chorus / Outro      (EDM)
;;   mood 2 "mid"   -> Intro / Verse 1-6 / Bridge / Chorus / Outro
;;   mood 3 "low"   -> Intro / Verse 1-2 / Bridge / Chorus / Outro
;;
;; Hvorfor det aldrig virkede foer runde 169 (0 af 342 numre i cachen havde
;; faser): (1) AnalysisTagFinder henter KUN de tags nogen har meldt interesse
;; for med addAnalysisTagListener - uden en lytter paa PSSI/.EXT var dens
;; cache altid tom; (2) koden laeste mood/entries paa SongStructureTag i
;; stedet for SongStructureBody, og kind-structen blev aldrig pakket helt ud
;; til et tal; (3) et nummer der var klargjort paa det andet deck laa i
;; cachen med VORES analyse, og saa blev rekordbox aldrig spurgt.

(def qlc-phrase-mood-names {1 "high" 2 "mid" 3 "low"})

(defn qlc-kaitai-num
  "Et Kaitai-felt som et tal. Kan vaere et tal, en enum (.id -> long) eller en
   struct der holder en enum (.id -> enum -> .id -> long) - pakkes ud indtil
   der staar et tal, hoejst fire lag."
  [x]
  (loop [x x depth 0]
    (cond
      (nil? x)    nil
      (number? x) (long x)
      (> depth 3) nil
      :else       (let [y (try (.id x) (catch Throwable _ ::ingen))]
                    (if (= y ::ingen) nil (recur y (inc depth)))))))

(defn qlc-phrase-type
  "Rekordbox' phrase-id oversat til et af vores flag.

   Up bliver til build, Down og Bridge til break, Chorus til drop, og Intro og
   Outro til sig selv. Alt andet (versene) er normal - de er der, men de skal
   ikke lave om paa lyset."
  [mood kind]
  (let [m (or mood 1) k (or kind 0)]
    (if (= m 1)
      (case k 1 "intro" 2 "build" 3 "break" 5 "drop" 6 "outro" "normal")
      (case k 1 "intro" 8 "break" 9 "drop" 10 "outro" "normal"))))

(def qlc-pssi-listener (atom nil))    ; lytteren vi har meldt til finderen
(def qlc-pssi-started (atom 0))       ; sidste gang vi proevede at starte den

(defn qlc-pssi-finder
  "AnalysisTagFinder'en, koerende og med en lytter paa PSSI i .EXT, eller nil.

   Lytteren goer ingenting. Den er der fordi finderen kun henter de tags nogen
   har bedt om: med den henter den PSSI for hvert nummer der bliver lagt paa
   et daek, og getLatestTrackAnalysisFor finder den bagefter uden at vente.
   Den startes hvis den ikke koerer (hoejst hvert tiende sekund - den stopper
   selv naar BLT gaar offline, og skal startes igen naar den kommer tilbage);
   lytteren overlever et stop.

   Class/forName og en java.lang.reflect.Proxy, ikke reify og ikke en
   static-form: de bliver slaaet op naar filen KOMPILERES, og en beat-link
   uden klassen ville faa hele namespacet til at fejle."
  []
  (try
    (let [cls    (Class/forName "org.deepsymmetry.beatlink.data.AnalysisTagFinder")
          finder (clojure.lang.Reflector/invokeStaticMethod cls "getInstance" (object-array 0))]
      (when (and (not (.isRunning finder))
                 (> (- (System/currentTimeMillis) @qlc-pssi-started) 10000))
        (reset! qlc-pssi-started (System/currentTimeMillis))
        (try (.start finder)
             (catch Throwable e
               (try (timbre/warn "QLC+ kunne ikke starte AnalysisTagFinder:" (.getMessage e))
                    (catch Throwable _ nil)))))
      (when (nil? @qlc-pssi-listener)
        (let [iface   (Class/forName "org.deepsymmetry.beatlink.data.AnalysisTagListener")
              handler (reify java.lang.reflect.InvocationHandler
                        (invoke [_ self method args]
                          ;; lytteren ligger i et hash-set: hashCode og equals
                          ;; SKAL svare, ellers kaster finderen ved tilmeldingen
                          (case (.getName ^java.lang.reflect.Method method)
                            "hashCode" (int (System/identityHashCode self))
                            "equals"   (boolean (identical? self (aget ^objects args 0)))
                            "toString" "QLC+ PSSI listener"
                            nil)))
              l       (java.lang.reflect.Proxy/newProxyInstance
                        (.getClassLoader iface) (into-array Class [iface]) handler)]
          ;; compare-and-set: to triggers kunne ellers melde to lyttere til
          (when (compare-and-set! qlc-pssi-listener nil l)
            (.addAnalysisTagListener finder l ".EXT" "PSSI")
            (try (timbre/info "QLC+ asks beat-link for rekordbox phrases (PSSI)")
                 (catch Throwable _ nil)))))
      (when (.isRunning finder) finder))
    (catch Throwable _ nil)))

(defn qlc-pssi-body
  "SongStructureBody ud af en PSSI-tags .body, eller nil."
  [x]
  (when x
    (or (try (let [b (.body x)] (.mood b) b) (catch Throwable _ nil))   ; crate-digger med afmaskning
        (try (.mood x) x (catch Throwable _ nil)))))                     ; aeldre: felterne paa taggen

(defn qlc-phrase-tag
  "PSSI-kroppen for nummeret paa den spiller, eller nil.

   ask? false: kun finderens cache - et opslag i et map, sikkert fra triggeren
   fem gange i sekundet. ask? true: er cachen tom, spoerg afspilleren (eller
   crate-digger paa USB'en) direkte. Det er et netvaerkskald og maa kun ske i
   en future (klargoeringen), aldrig i triggeren."
  [player ask?]
  (try
    (when-let [finder (qlc-pssi-finder)]
      (let [tag (or (.getLatestTrackAnalysisFor finder (int player) ".EXT" "PSSI")
                    (when ask?
                      (let [mf  (org.deepsymmetry.beatlink.data.MetadataFinder/getInstance)
                            md  (.getLatestMetadataFor mf (int player))
                            ref (when md (.-trackReference md))]
                        (when ref (.requestAnalysisTagFrom finder ref ".EXT" "PSSI")))))]
        (when tag (qlc-pssi-body (.body tag)))))
    (catch Throwable _ nil)))

(defn qlc-phrase-markers
  "Rekordbox' faser som vores markoerer, eller nil hvis der ingen er.

   curves maa vaere en delay: kurverne regnes kun ud hvis der er faser.
   Energien kommer fra vores egen kurve - rekordbox har ingen - saa motoren
   faar det samme at arbejde med som ellers. Der bliver ikke lagt et build
   foran hvert drop her: rekordbox har allerede sagt hvor Up'et starter, og
   det er praecis hvad et build er.

   Et saet der kun er normal/intro/outro (et nummer af bare vers) bruges
   ikke: det siger intet lyset kan bruge, og det er ogsaa praecis hvad en
   forkert laest tag ville give. Saa faar nummeret vores egen analyse."
  ([player curves] (qlc-phrase-markers player curves false))
  ([player curves ask?]
   (try
     (when-let [body (qlc-phrase-tag player ask?)]
       (let [mood    (qlc-kaitai-num (.mood body))
             entries (seq (.entries body))
             energy  (:energy (force curves))
             n       (count energy)
             end     (or (qlc-kaitai-num (.endBeat body)) n)
             raw     (->> entries
                          (keep (fn [e]
                                  (let [b (qlc-kaitai-num (.beat e))
                                        k (qlc-kaitai-num (.kind e))]
                                    (when (and b (pos? b))
                                      {:beat (long b)
                                       :type (qlc-phrase-type mood k)}))))
                          (sort-by :beat)
                          vec)]
         (when (and (seq raw) (pos? n))
           ;; energien pr. sektion, maalt paa vores egen kurve
           ;; inc: en sidste fase der slutter paa sit eget startbeat giver et
           ;; tomt interval, og saa faar den energi 0 - en doed outro
           (let [ends (conj (mapv :beat (rest raw)) (max end (inc (:beat (last raw)))))
                 out  (mapv (fn [m e]
                              (assoc m :energy
                                     (double (qlc-mean-range energy (dec (:beat m)) (dec e)))
                                     :source "rekordbox"))
                            raw ends)
                 ;; to flag paa samme beat, og flag efter sidste beat, hjaelper ingen
                 out  (->> out
                           (filter #(<= 1 (:beat %) (max 1 n)))
                           (partition-by :beat)
                           (mapv last))
                 useful (some #(contains? #{"break" "build" "drop"} (:type %)) out)]
             (try (timbre/info "QLC+ rekordbox phrases:" (count out) "faser,"
                               "mood" (get qlc-phrase-mood-names mood mood)
                               (if useful "" "- kun vers, vores analyse bruges"))
                  (catch Throwable _ nil))
             (when (and useful (>= (count out) 2))
               out)))))
     (catch Throwable e
       (try (timbre/warn "QLC+ kunne ikke laese rekordbox-faser:" (.getMessage e))
            (catch Throwable _ nil))
       nil))))

;; runde 357: rekordbox' fill-ins. Fasen (SongStructureEntry) har et
;; fill-flag og det beat hvor fill-in'et starter - trommehvirvlen i frasens
;; sidste 2-4 slag (264 af 1.425 faser i biblioteket). QLC+ hugger i
;; sekstendedele i dem. Boelgeformen kan ikke hoere en hvirvel selv: dens
;; anslag laa paa 6,3-6,5 pr. slag i alle sektioner og steg ikke foer 163 drops.
;; Kun laest fra finderens cache (ask? false) - intet skrives til CDJ'erne.
(defn qlc-phrase-fills
  "[[foerste-beat slut-beat] ...] for hver fase med et fill-in, eller []."
  [player]
  (try
    (if-let [body (qlc-phrase-tag player false)]
      (let [es  (vec (seq (.entries body)))
            end (qlc-kaitai-num (.endBeat body))]
        (vec (keep-indexed
               (fn [i e]
                 (let [f  (try (qlc-kaitai-num (.fill e)) (catch Throwable _ nil))
                       bf (try (qlc-kaitai-num (.beatFill e)) (catch Throwable _ nil))
                       nx (if (< (inc i) (count es))
                            (qlc-kaitai-num (.beat (nth es (inc i))))
                            end)]
                   (when (and f (pos? f) bf (pos? bf) nx (> nx bf) (<= (- nx bf) 16))
                     [(long bf) (long nx)])))
               es)))
      [])
    (catch Throwable _ [])))

(defn qlc-hand-made?
  "Er cache-entry'en en haandrettelse? Rekordbox-faser gemt af en aeldre
   version var ogsaa :manual - de har :rekordbox og taeller ikke."
  [e]
  (boolean (and (:manual e) (not (:rekordbox e)))))

;; ------------------------------------------------- analyse 10: sektioner fra kicket
;;
;; Tobias, 2026-09-27: "den rammer de fleste drops, men stort set alle andre
;; sektioner lander ofte forkert." Maalt mod hans haandrettede flag (fem numre
;; hvor den praecise version ligger i rekordbox-biblioteket, genskabt fra
;; ANLZ-filerne med en Python-kopi af analysen - analysis_eval/ i mappen):
;;
;;                          graenser ramt (+-1 takt)   typer, beat for beat
;;   analyse 9               47 %                       55 %  (drop 50 %)
;;   rekordbox' faser        38 %                       58 %  (normal 0 %)
;;   analyse 10              66 %                       72 %  (drop 90 %)
;;
;; Hvad hans flag viser: et break starter hvor kicket forsvinder, et drop hvor
;; det kommer tilbage, et build er den sidste del (2-8 takter) af den
;; kick-loese straekning foer et drop, og drop og groove har begge kick og
;; skilles paa energien. Den gamle analyse ledte efter energiovergange paa et
;; fast 8-takts-gitter og ramte ved siden af hver gang kicket gik 2 takter
;; foer frasens linje; og den kaldte 44 % af hans drops "drive", fordi et drop
;; skulle have et build foran sig. Han bruger aldrig drive selv - analyse 10
;; siger drop eller normal efter energien.
;;
;; rekordbox' faser giver nu kun linjerne en kick-straekning deles paa (drop
;; mod groove). Deres typer var det der ikke holdt: "Up" blev build over
;; 40 takter, og mood high har ingen groove.

(defn qlc-kick-runs
  "Takterne som [kick? fra til) i takt-indeks, renset: kick i hoejst
   :kick-blip-bars takter midt i et break er et break, og et hul paa én takt
   midt i kicket er et break kun naar det staar alene (et hul i hver ottende
   takt er frasens fill) og kicket varer mindst fire takter paa begge sider."
  [kbar]
  (let [runs0 (reduce (fn [acc [b k]]
                        (let [l (peek acc)]
                          (if (and l (= (nth l 0) k))
                            (conj (pop acc) [k (nth l 1) (inc b)])
                            (conj acc [k b (inc b)]))))
                      [] (map-indexed vector kbar))
        blip  (qlc-opt :kick-blip-bars 2)
        gap   (qlc-opt :kick-min-gap-bars 2)
        iso   (qlc-opt :kick-gap-iso-bars 8)
        fold? (fn [runs j]
                (let [[k a e]    (nth runs j)
                      [kp ap ep] (nth runs (dec j))
                      [kn an en] (nth runs (inc j))
                      L          (- e a)]
                  (and (= kp kn) (not= kp k)
                       (or (and k (<= L blip))
                           ;; analyse 10c: a drop -> one bar -> drop restarted the
                           ;; whole look twice in four beats (the engine takes
                           ;; every flag as a new part) - the drop keeps it
                           (and (not k) (< L gap))
                           (and (not k) (= L 1)
                                (or (some (fn [i]
                                            (let [[kr ar _] (nth runs i)]
                                              (and (not= i j) (not kr)
                                                   (<= (Math/abs (long (- ar a))) iso))))
                                          (range (count runs)))
                                    (< (- ep ap) 4)
                                    (< (- en an) 4)))))))
        fold  (fn [runs j]
                (let [[k0 a0 _] (nth runs (dec j))
                      [_ _ e2]  (nth runs (inc j))]
                  (vec (concat (subvec runs 0 (dec j)) [[k0 a0 e2]] (subvec runs (+ j 2))))))]
    (loop [runs runs0 guard 0]
      (let [j (when (< guard 10000)
                (first (filter #(fold? runs %) (range 1 (dec (count runs))))))]
        (if j (recur (fold runs j) (inc guard)) runs)))))

(defn qlc-kick-blocks
  "En kick-straekning [a e) (takter) delt i drop og normal efter energien:
   paa rekordbox' fraselinjer naar de er givet (cuts, kan vaere tom), ellers i
   blokke paa :kick-block-bars takter fra straekningens start.

   edm (10g) er nil eller {:kind-at f :next-after f} over rekordbox' faser (i
   takter): saa er en Chorus-blok drop, et Up der loeber lige ind i et Chorus
   er build i sine sidste takter, og resten er normal. Paa trance og big room
   er opbygningen MED kick ofte 80 % af droppets energi - energien alene
   kaldte hele Up'et drop, og droppet landede 8-16 takter for tidligt."
  ([ebar emax a e cuts] (qlc-kick-blocks ebar emax a e cuts nil))
  ([ebar emax a e cuts edm]
  (let [bb  (qlc-opt :kick-block-bars 8)
        dr  (qlc-opt :kick-drop-rel 0.70)
        ub  (qlc-opt :kick-edm-up-build-bars 8)
        cls (fn [x y]
              (let [er (/ (/ (reduce + (subvec ebar x y)) (- y x)) emax)]
                (if edm
                  ;; a run that starts a bar before a phrase line takes that
                  ;; line's type: the cut rule (a+2) never splits there, and
                  ;; a kick a bar before the Chorus lost the drop (fejljagt 3)
                  (let [k0   (if (= x a) (inc x) x)
                        kind ((:kind-at edm) k0)]
                    (if (= kind 5)
                      (if (>= er (qlc-opt :kick-edm-chorus-floor 0.75)) "drop" "normal")
                      (cond
                        (>= er (qlc-opt :kick-edm-other-drop 0.97)) "drop"
                        (and (= kind 2)
                             (let [[nk pb] ((:next-after edm) k0)] (and (= nk 5) (= pb y)))) "build"
                        :else "normal")))
                  (if (>= er dr) "drop" "normal"))))]
    (loop [x a out []]
      (if (>= x e)
        ;; a run-up is only a build when the drop follows it in this run - a
        ;; Chorus that came out "normal", or a kick pause before it, left a
        ;; build that led nowhere (fejljagt 3)
        (if edm
          (vec (map-indexed (fn [i [t x0 y0]]
                              (if (and (= t "build") (not= "drop" (get-in out [(inc i) 0])))
                                ["normal" x0 y0]
                                [t x0 y0]))
                            out))
          out)
        (let [y (if cuts
                  (or (first (filter #(> % x) cuts)) e)
                  (let [y (min e (+ x bb))]
                    ;; en kort hale hoerer til blokken foer den
                    (if (< (- e y) (quot bb 2)) e y)))
              c (cls x y)]
          (recur y (if (and (= c "build") (> (- y x) ub))
                     (conj out ["normal" x (- y ub)] ["build" (- y ub) y])
                     (conj out [c x y])))))))))

(defn qlc-kick-loud-gap?
  "R505_SECT_LOUDGAP: har analyse 10 lagt et break paa mindst :kick-loud-gap-bars
   takter midt i nummeret, med energi over :kick-loud-gap-rel af droppenes? Saa
   har kick-maalingen tabt kicket (ikke-4/4, brudt beat, latin, disco): cachen
   25-10 har det i 17 af 523 automatiske numre (et break paa 99 takter i
   'Make you mine', 57 i 'Think of You' - Tobias rettede dem live 8 + 1 gange
   natten 24-25), og i 0 af 119 haandrettede og 0 af 90 rekordbox."
  [mk n]
  (let [ends  (conj (mapv :beat (rest mk)) (inc n))
        secs  (mapv (fn [m e] (assoc m :len (- e (:beat m)))) mk ends)
        drops (filter #(= "drop" (:type %)) secs)
        dl    (reduce + 0 (map :len drops))]
    (when (pos? dl)
      (let [de (/ (reduce + 0.0 (map #(* (double (:energy %)) (:len %)) drops)) dl)]
        (boolean
          (some (fn [[i s]]
                  ;; R520_TRACKANA_LOUDGAP_FIRST: ogsaa den FOERSTE sektion (ikke den sidste:
                  ;; en outro maa vaere lang). Et kick-loest "break" paa 32+ takter i starten
                  ;; med droppenes energi er kicket tabt (half-time/171->128-overgang, salsa) -
                  ;; cachen 31-10: 3 af 538 automatiske, 0 af 124 haandrettede, 0 af 90 rekordbox
                  (and (= "break" (:type s)) (< i (dec (count secs)))
                       (>= (:len s) (* 4 (qlc-opt :kick-loud-gap-bars 32)))
                       (>= (double (:energy s)) (* (qlc-opt :kick-loud-gap-rel 0.60) de))))
                (map-indexed vector secs)))))))

(defn qlc-kick-markers
  "Analyse 10: sektionerne fra hvor kicket er, takt for takt. phrase-beats er
   rekordbox' fraselinjer (1-baserede beats) eller nil. nil hvis nummeret
   ikke har et kick at dele efter - saa bruges den gamle analyse."
  ([curves grid phrase-beats] (qlc-kick-markers curves grid phrase-beats nil))
  ([curves grid phrase-beats edm-entries]
  (let [kn     (:kick-raw curves)
        energy (:energy curves)
        roll   (:roll curves)
        n      (count kn)
        db     (qlc-downbeat grid n)
        bars   (quot (- n db) 4)]
    (when (and (seq kn) (>= bars 16))
      (let [kt     (qlc-kick-threshold kn)
            on     (mapv #(if (> % kt) 1 0) kn)
            counts (mapv (fn [b] (reduce + (subvec on (+ db (* 4 b)) (+ db (* 4 b) 4)))) (range bars))
            ;; hvordan en kick-takt ser ud i DET HER nummer: fire paa gulvet,
            ;; eller halvtakt/brudt med to - "3 af 4" saa intet kick i dem og
            ;; gjorde hele nummeret til én sektion
            nz     (filter pos? counts)
            full   (if (seq nz) (qlc-percentile nz 75) 4)
            need   (max 1 (min (qlc-opt :kick-on-beats 3) (Math/round (* 0.75 (double full)))))
            kbar   (mapv #(>= % need) counts)
            ebar   (mapv (fn [b] (qlc-mean-range energy (+ db (* 4 b)) (+ db (* 4 b) 4))) (range bars))
            runs   (qlc-kick-runs kbar)
            ;; nummerets top: de hoejeste takter MED kick
            kb     (vec (keep-indexed (fn [b k] (when k (ebar b))) kbar))
            emax   (max 1e-6 (if (seq kb) (qlc-percentile kb 90) (reduce max 1e-6 ebar)))
            cutset (when (seq phrase-beats)
                     (set (map #(long (Math/rint (/ (double (- (long %) 1 db)) 4.0))) phrase-beats)))
            ;; rekordbox' faser i takter, [takt type] sorteret (10g)
            pk     (when (seq edm-entries)
                     (vec (sort (map (fn [[b k]] [(long (Math/rint (/ (double (- (long b) 1 db)) 4.0))) (long k)])
                                     edm-entries))))
            edm    (when pk
                     {:kind-at    (fn [bar] (second (last (filter #(<= (first %) bar) pk))))
                      :next-after (fn [bar] (let [[pb k] (first (filter #(> (first %) bar) pk))]
                                              (when pb [k pb])))})
            secs   (reduce (fn [acc [k a e]]
                             (if-not k
                               (conj acc ["break" a e])
                               (let [cuts (when cutset
                                            (vec (sort (filter #(and (>= % (+ a 2)) (< % (dec e))) cutset))))]
                                 (reduce (fn [acc2 [t x y]]
                                           (let [l (peek acc2)]
                                             (if (and l (= (nth l 0) t) (= (nth l 2) x))
                                               (conj (pop acc2) [t (nth l 1) y])
                                               (conj acc2 [t x y]))))
                                         acc (qlc-kick-blocks ebar emax a e cuts edm)))))
                           [] runs)
            bmin   (qlc-opt :kick-build-min-bars 2)
            bmax   (qlc-opt :kick-build-max-bars 8)
            nsec   (count secs)
            out    (vec (mapcat
                          (fn [i]
                            (let [[t a e :as s] (nth secs i)
                                  nxt (when (< (inc i) nsec) (nth secs (inc i)))
                                  prv (when (pos? i) (nth secs (dec i)))
                                  L   (- e a)]
                              (cond
                                ;; build'et er halen af breaket foer et drop: hvor
                                ;; rollen eller energien begynder at stige, ellers
                                ;; den sidste halvdel - 2-8 takter
                                ;; (not a short gap between two drops: that is a
                                ;; drop-out, and a two-bar "build" there dipped the
                                ;; level and swapped programmes three times - 10c)
                                (and nxt (= "drop" (nth nxt 0)) (= t "break")
                                     (not (and prv (= "drop" (nth prv 0))
                                               (< L (qlc-opt :kick-drop-gap-bars 3)))))
                                (let [d  (+ db (* 4 e))
                                      lo (+ db (* 4 a))
                                      bs (or (qlc-roll-start roll d lo) (qlc-build-start energy d lo))
                                      bb (if bs (max 1 (min bmax (quot (+ (- d bs) 2) 4))) (quot L 2))
                                      bb (max bmin (min bmax bb))]
                                  (cond
                                    (< L bmin) [s]      ; et hul paa én takt er et drop-out, ikke en opbygning
                                    (>= bb L)  [["build" a e]]
                                    :else      [["break" a (- e bb)] ["build" (- e bb) e]]))
                                ;; kort kick mellem et break og et drop, lavere end
                                ;; drop'et: opbygningen med kick
                                (and nxt (= "drop" (nth nxt 0)) prv (= "break" (nth prv 0))
                                     (= t "normal") (<= L (qlc-opt :kick-short-on-bars 4)))
                                [["build" a e]]
                                :else [s])))
                          (range nsec)))
            res    (reduce (fn [acc [t a e]]
                             (let [l (peek acc)]
                               (if (and l (= (nth l 0) t))
                                 (conj (pop acc) [t (nth l 1) e])
                                 (conj acc [t a e]))))
                           [] out)
            mk     (vec (map-indexed (fn [j [t a e]]
                                       {:beat   (if (zero? j) 1 (+ db (* 4 a) 1))
                                        :type   t
                                        :energy (double (qlc-mean-range energy (+ db (* 4 a)) (+ db (* 4 e))))})
                                     res))]
        (when (and (>= (count mk) 3)
                   (some #(= "drop" (:type %)) mk)
                   ;; analyse 10f, 400 tilfaeldige numre fra biblioteket: rock,
                   ;; pop og hiphop med kick i 20-30 % af takterne blev 65-84 %
                   ;; "break" - trommerne er der, bare ikke paa alle slag. Under
                   ;; 30 % er kicket ikke nummerets rygrad; Tobias' 62 laveste: 37 %
                   (>= (count kb) (* (qlc-opt :kick-min-share 0.30) bars))
                   ;; R505_SECT_LOUDGAP_USE: et hoejt break paa 32+ takter = kicket er tabt - den gamle analyse
                   (not (qlc-kick-loud-gap? mk n)))
          mk))))))

(defn qlc-phrase-entries
  "rekordbox' faser for nummeret paa spilleren som {:mood m :entries [[beat
   type] ...]} i rekordbox' egen raekkefoelge, eller nil. ask? som i
   qlc-phrase-tag (10g)."
  [player ask?]
  (try
    (when-let [body (qlc-phrase-tag player ask?)]
      (let [es (vec (keep (fn [e] (let [b (qlc-kaitai-num (.beat e)) k (qlc-kaitai-num (.kind e))]
                                    (when (and b (pos? b)) [b (or k 0)])))
                          (.entries body)))]
        (when (seq es) {:mood (qlc-kaitai-num (.mood body)) :entries es})))
    (catch Throwable _ nil)))

(defn qlc-edm-form?
  "rekordbox' EDM-form: mood high og mindst én Down -> Up -> Chorus (10g)."
  [ph]
  (boolean (and ph (= 1 (:mood ph))
                (some (fn [[a b c]] (and (= 3 a) (= 2 b) (= 5 c)))
                      (partition 3 1 (map second (:entries ph)))))))

(defn qlc-auto-markers
  "Vores analyse af et nummer, som den gemmes og sendes: analyse 10 (kicket,
   delt paa rekordbox' fraselinjer hvor de findes). Har nummeret intet kick at
   dele efter, rekordbox' egne faser, og ellers den gamle analyse."
  [player detail grid curves ask?]
  ;; ^{:kick true}: QLC+ skips its second pass only for the kick analysis -
  ;; the fallbacks below are the old analysis and still want it
  ;; ... and the fallbacks say so ({:kick false}): qlc-cache-put! keeps an
  ;; entry's :kick when a set comes WITHOUT the mark (QLC+'s UNDO, 10c)
  (if-let [m (let [ph (qlc-phrase-entries player ask?)]
               ;; EDM (10g): rekordbox' Chorus/Up inde i kick-straekningerne
               (qlc-kick-markers curves grid (seq (map first (:entries ph)))
                                 (when (qlc-edm-form? ph) (:entries ph))))]
    (with-meta m {:kick true})
    (let [m (or (qlc-phrase-markers player curves ask?)
                (let [c2 (assoc curves :patent (qlc-patent-for detail grid curves))]
                  (qlc-build-markers c2 grid (qlc-cue-beats grid player))))]
      (when m (with-meta (vec m) {:kick false})))))

;; ---------------------------------------------------------------- R525_ML_BLOCK: sektioner laert af Time-line
;;
;; Tobias 11-01 ("Synes vi skal lave alle punkter"): en model laert paa 577 numre med Time-line Lives
;; sektioner som facit. Paa 61 numre den aldrig saa: 84-85 % samme tilstand som Time-line, mod 72-74 %
;; for analyse 10 (92 % hvor Time-line selv er sikker). Den laeser KUN det BLT har i forvejen: kurverne
;; fra qlc-curves (rekordbox' farveboelgeform + beatgrid). Pr. takt: takten, dens naboer og
;; straekningerne omkring den (+-16 takter), z-scoret i nummeret selv, plus hvor i nummeret takten
;; staar. Traeerne (gradient boosting) ligger i qlc-ml-model-text; Viterbi glatter (et skift koster,
;; mindre paa en 4-takters linje). Ingen ny Java, intet skrives til CDJ'erne.
;;
;; Saettet sendes VED SIDEN AF vores eget (:markersAi) og gemmes kun i hukommelsen: cachen paa disken,
;; haandrettelserne og analyse 10 er som foer. QLC+ vaelger (SETUP > ADVANCED, trackengine/ai-sections);
;; en aeldre QLC+ ser bort fra feltet. Haandrettede flag vinder altid.

(def qlc-ml-analysis 15)   ; det QLC+ faar som :analysisAi - saettet er endeligt (ingen anden gennemgang)

(def qlc-ml-curve-keys [:display :energy :kick :low :mid :high :roll :kick-raw :high-raw :mid-raw :pump-raw])
(def qlc-ml-pack-keys [:onset-high :onset-low :onset-mid])

(defn qlc-ml-pct
  "numpy's lineaere percentil (default) paa en sorteret double-array"
  ^double [^doubles s q]
  (let [n (alength s)]
    (if (zero? n)
      0.0
      (let [pos (* (/ (double q) 100.0) (dec n))
            lo  (long (Math/floor pos))
            hi  (min (dec n) (inc lo))
            fr  (- pos lo)]
        (+ (aget s lo) (* fr (- (aget s hi) (aget s lo))))))))

(defn qlc-ml-beat-rows
  "pr. beat: kurverne og de pakkede anslag (4 nibbles pr. beat, tre baand) som tal"
  [curves n]
  (let [cv (mapv (fn [k] (vec (get curves k))) qlc-ml-curve-keys)
        pv (mapv (fn [k] (vec (get curves k))) qlc-ml-pack-keys)]
    (mapv (fn [i]
            (double-array
              (concat (map (fn [c] (double (if (< i (count c)) (or (nth c i) 0.0) 0.0))) cv)
                      (mapcat (fn [c]
                                (let [x (long (if (< i (count c)) (or (nth c i) 0) 0))]
                                  (map (fn [q] (double (bit-and (bit-shift-right x (* 4 q)) 15))) (range 4))))
                              pv))))
          (range n))))

(defn qlc-ml-bar-starts
  "takternes foerste beat (0-baseret) efter rekordbox' beat-i-takt; foer foerste downbeat en takt for sig"
  [grid n]
  (let [s (vec (filter (fn [i] (= 1 (.getBeatWithinBar grid (int (inc i))))) (range n)))]
    (if (or (empty? s) (pos? (first s))) (vec (cons 0 s)) s)))

(defn qlc-ml-features
  "{:starts [takt-starter 0-baseret] :X [[featurevektor pr. takt]]} - samme regning som traeningen"
  [curves grid]
  (let [n      (long (.-beatCount grid))
        rows   (qlc-ml-beat-rows curves n)
        starts (qlc-ml-bar-starts grid n)
        m      (count starts)
        F      (alength ^doubles (first rows))
        B      (mapv (fn [j]
                       (let [s (nth starts j)
                             e (if (< (inc j) m) (nth starts (inc j)) n)
                             e (max e (inc s))
                             acc (double-array F)]
                         (doseq [i (range s e) f (range F)]
                           (aset acc f (+ (aget acc f) (aget ^doubles (nth rows i) f))))
                         (amap acc f _ (/ (aget acc f) (double (- e s))))))
                     (range m))
        col    (fn [f] (let [a (double-array (map #(aget ^doubles % f) B))] (java.util.Arrays/sort a) a))
        stats  (mapv (fn [f] (let [s (col f)]
                               [(qlc-ml-pct s 50) (+ (- (qlc-ml-pct s 75) (qlc-ml-pct s 25)) 1e-6) (qlc-ml-pct s 90)]))
                     (range F))
        Z      (mapv (fn [b] (double-array (map (fn [f] (let [[med iqr] (nth stats f)] (/ (- (aget ^doubles b f) med) iqr))) (range F)))) B)
        R      (mapv (fn [b] (double-array (map (fn [f] (/ (aget ^doubles b f) (+ (Math/abs (double (nth (nth stats f) 2))) 1e-6))) (range F)))) B)
        win    (fn [a b]
                 (mapv (fn [k]
                         (let [lo (max 0 (+ k a)) hi (min m (+ k b))]
                           (if (> hi lo)
                             (let [acc (double-array F)]
                               (doseq [j (range lo hi) f (range F)]
                                 (aset acc f (+ (aget acc f) (aget ^doubles (nth Z j) f))))
                               (amap acc f _ (/ (aget acc f) (double (- hi lo)))))
                             (aclone ^doubles (nth Z k)))))
                       (range m)))
        W      (mapv (fn [[a b]] (win a b)) [[-1 0] [1 2] [-4 0] [1 5] [-8 -4] [5 9] [-16 -8] [9 17]])
        [p4 n4] [(nth W 2) (nth W 3)]
        X      (mapv (fn [k]
                       (let [z (nth Z k)]
                         (vec (concat (seq z) (seq ^doubles (nth R k))
                                      (mapcat (fn [w] (seq ^doubles (nth w k))) W)
                                      (map (fn [f] (- (aget ^doubles z f) (aget ^doubles (nth p4 k) f))) (range F))
                                      (map (fn [f] (- (aget ^doubles (nth n4 k) f) (aget ^doubles z f))) (range F))
                                      [(double (mod k 4)) (double (mod k 8)) (double (mod k 16)) (double (mod k 32))
                                       (/ (double k) (double (max 1 (dec m)))) (double (- m k)) (double k)]))))
                     (range m))]
    {:starts starts :X X}))

(def qlc-ml-model-text
  ;; traeerne som tekst: antal tilstande, navnene, antal runder, startvaerdierne, og pr. trae dets
  ;; knuder (feature taerskel venstre hoejre, eller -1 vaerdi for et blad). Delt i korte stykker:
  ;; en streng over 64 KB kan Java ikke have som konstant
  (clojure.string/join " " [
   "4 BREAK BUILD DROP GROOVE 120 -0.257599 -0.672355 1.03796 -0.108003 29 25 0.1608111 1 22 28 0.4146214 2 11 34 0.4799999 3 8 255 0.2521934 4 7 31 0.1008963 5 6 -1 0.489206 -1 0.40358 -1 0.229714 148 -0.007924817 9 10 -1 0.313343 -1 0.102652 164 -0.0005104494 12 19 128 -0.09467455 13 16 34 0.745098 14 15 -1 0.312744 -1 0.159782 280 0.619469 17 18 -1 0.189558 -1 0.0554004 28 0.5806807 20 21 -1 0.127158 -1 0.0118407 25 0.3919926 23 28 34 0.5121951 24 27 126 -0.5499996 25 26 -1 0.203823 -1 0.00937769 -1 -0.0346359 -1 -0.0912631 29 25 0.4421701 1 24 208 0.0284127 2 9 124 -0.3306193 3 4 -1 -0.0387205 192 -0.1841334 5 8 119 0.4280733 6 7 -1 0.125067 -1 0.322757 -1 0.0493351 232 -0.2403536 10 13 235 0.3809404 11 12 -1 0.102747 -1 0.34387 164 0.02753455 14 19 123 -0.06997248 15 16 -1 0.141673 191 -0.3683841 17 18 -1 0.449117 -1 0.252304 26 0.8380342 20 23 28 0.4297295 21 22 -1 0.290008 -1 0.564745"
   "-1 -0.00956854 26 0.5210106 25 28 208 0.1530587 26 27 -1 -0.0316145 -1 0.242747 -1 -0.0771696 29 24 0.6901972 1 16 95 0.004887994 2 9 25 0.173618 3 4 -1 -0.174199 41 0.1999998 5 6 -1 -0.150994 24 0.5734288 7 8 -1 -0.108269 -1 -0.0338964 41 0.1875 10 13 256 0.266086 11 12 -1 -0.130953 -1 -0.000541591 25 0.12173 14 15 -1 -0.125866 -1 0.0117256 24 0.8013504 17 24 41 0.09090906 18 19 -1 -0.071381 25 0.3095682 20 21 -1 -0.0899948 282 15 22 23 -1 -0.0504644 -1 0.0845329 25 0.3671716 25 26 -1 0.00755295 5 -0.3029358 27 28 -1 0.0719744 -1 0.130579 29 33 0.9973108 1 12 102 0.3027648 2 9 24 0.808271 3 8 30 0.08637716 4 5 -1 -0.0689435 162 -0.02530453 6 7 -1 0.0479635 -1 -0.0402046 -1 -0.0858045 33 0.9833324 10 11 -1 0.00656595 -1 0.123375 124 -0.2431164 13 24 38 0.09070294 14 17 42 0.6785714 15 16 -1 -0.0122176 -1 0.210222 41 0.09999998 18 19 -1 0.377379 282 15 20 21 -1 0.354172 26 0.9031742 22 23"
   "-1 0.2051 -1 0.0426616 280 0.1486486 25 26 -1 0.197533 26 0.9209767 27 28 -1 0.0422394 -1 -0.0692018 29 25 0.1608111 1 22 34 0.4452054 2 11 128 -0.1578947 3 6 163 0.004415888 4 5 -1 0.279533 -1 0.172287 115 -0.07787551 7 10 31 0.2467786 8 9 -1 0.249601 -1 0.135614 -1 0.0382474 163 -0.01538911 12 19 187 -0.7068829 13 14 -1 0.0677826 36 0.5853658 15 16 -1 0.205575 25 0.06578656 17 18 -1 0.15563 -1 0.0399078 5 -0.0007017965 20 21 -1 0.0760461 -1 -0.0262086 25 0.4180715 23 28 36 0.3636363 24 27 88 -0.5999995 25 26 -1 0.175394 -1 0.0140578 -1 -0.0337597 -1 -0.0893646 29 25 0.4842724 1 24 208 0.01234435 2 9 235 0.1078282 3 8 124 -0.3225856 4 5 -1 -0.0525537 185 -0.04660819 6 7 -1 0.0900263 -1 -0.00726901 -1 0.112879 139 0.06671255 10 21 123 -0.3103256 11 14 50 -0.2205247 12 13 -1 0.0726787 -1 0.200858 26 0.6465874 15 20 281 84 16 19 37 0.4166666 17 18 -1 0.254708 -1 0.357738 -1 0.221842 -1 0.0883981"
   "164 0.2308858 22 23 -1 0.0456495 -1 0.187801 26 0.4922176 25 28 169 0.09206775 26 27 -1 -0.0405002 -1 0.191916 -1 -0.0754002 29 24 0.6955917 1 16 49 0.01964895 2 13 25 0.1096933 3 4 -1 -0.149455 40 0.4242424 5 8 92 0.02417156 6 7 -1 -0.131288 -1 -0.0636359 231 -0.09220785 9 10 -1 -0.0151911 26 0.5851879 11 12 -1 -0.132798 -1 -0.0568284 40 0.2857141 14 15 -1 -0.0780649 -1 0.00956934 24 0.8013504 17 24 41 0.1578947 18 19 -1 -0.0531731 25 0.3273253 20 21 -1 -0.0721927 282 15 22 23 -1 -0.0366914 -1 0.0783169 25 0.3671716 25 26 -1 0.00813398 5 -0.3918797 27 28 -1 0.0548315 -1 0.117882 29 33 0.9972212 1 16 102 0.2673528 2 9 24 0.821184 3 8 30 0.07532204 4 5 -1 -0.0692855 162 0.01439242 6 7 -1 0.0296868 -1 -0.0462822 -1 -0.0831846 24 0.7866623 10 15 30 0.09759956 11 12 -1 -0.0253612 109 -0.1499999 13 14 -1 0.176065 -1 0.0449534 -1 -0.0409647 170 -0.1048508 17 26 240 0.01592386 18 21 24 0.808271"
   "19 20 -1 0.246581 -1 0.0367661 30 0.9770879 22 23 -1 0.0421868 26 0.9392938 24 25 -1 0.233422 -1 0.0626582 280 0.1486486 27 28 -1 0.149758 -1 -0.0173818 29 25 0.1910541 1 26 28 0.5330184 2 17 34 0.5882352 3 10 254 0.5967103 4 9 281 180 5 6 -1 0.20814 134 -0.4444442 7 8 -1 0.167889 -1 -0.0431347 -1 0.0944192 102 -0.04352742 11 12 -1 0.16882 281 111 13 16 280 0.7956989 14 15 -1 0.15428 -1 -0.0332102 -1 -0.0223939 164 -0.0005104494 18 25 36 0.5907172 19 22 280 0.8422939 20 21 -1 0.149462 -1 -0.038601 38 0.05555555 23 24 -1 0.135676 -1 0.0346692 -1 0.00310951 25 0.4180715 27 28 -1 -0.0203177 -1 -0.087443 29 25 0.4842724 1 24 161 -0.1967999 2 9 235 0.07600741 3 6 32 0.4550158 4 5 -1 -0.0500155 -1 0.0223122 207 -0.02127362 7 8 -1 0.0447765 -1 0.148756 139 -0.01403739 10 19 129 -0.1499999 11 14 27 0.4807364 12 13 -1 0.0397283 -1 0.15437 26 0.5983335 15 18 32 0.6249293 16 17 -1 0.181718 -1 0.253745"
   "-1 0.0642889 210 0.3404887 20 23 254 0.2148326 21 22 -1 0.00572933 -1 0.0905067 -1 0.170282 26 0.5851879 25 28 208 0.1530587 26 27 -1 -0.0473281 -1 0.127134 -1 -0.0742848 29 24 0.6901972 1 14 95 0.01976175 2 11 24 0.5535391 3 8 233 -0.4531695 4 7 118 -0.4837756 5 6 -1 -0.113103 -1 -0.0196276 -1 -0.130702 41 0.05555554 9 10 -1 -0.113608 -1 -0.0461526 40 0.2857141 12 13 -1 -0.0798309 -1 0.00286307 24 0.8013504 15 24 41 0.25 16 19 271 0.09374998 17 18 -1 -0.0836243 -1 0.021073 25 0.3095682 20 21 -1 -0.0681229 282 15 22 23 -1 -0.0317379 -1 0.0769489 74 -0.4837915 25 26 -1 0.0412655 25 0.3671716 27 28 -1 0.011392 -1 0.108184 29 33 0.9972212 1 14 102 0.1909086 2 9 115 0.08674491 3 8 33 0.9222661 4 5 -1 -0.0625935 23 0.7660607 6 7 -1 0.0124241 -1 -0.0543332 -1 -0.0810224 98 -0.4347791 10 11 -1 0.0824082 24 0.7976154 12 13 -1 0.0155686 -1 -0.0567996 216 -0.04397842 15 26 147 -0.2010876 16 21 56 0.1527012"
   "17 18 -1 0.103558 38 0.03333333 19 20 -1 0.0649331 -1 0.203375 133 0.1041666 22 25 30 0.08637716 23 24 -1 -0.0304305 -1 0.133404 -1 -0.030073 280 0.1486486 27 28 -1 0.123632 -1 -0.0188683 29 25 0.2223991 1 24 34 0.4799999 2 11 115 -0.02797283 3 8 37 0.4285714 4 5 -1 0.169231 34 0 6 7 -1 0.00429128 -1 0.116163 255 0.2521934 9 10 -1 0.106891 -1 -0.000355983 25 0.07665319 12 21 178 0.15625 13 18 141 -0.8927635 14 17 47 -1.058765 15 16 -1 0.108756 -1 -0.00100566 -1 0.127526 120 0.1982567 19 20 -1 0.0663787 -1 -0.02647 256 -0.04949535 22 23 -1 0.0755979 -1 -0.0150287 26 0.6122659 25 28 34 0.5333332 26 27 -1 0.0132179 -1 -0.0633898 -1 -0.0871815 29 30 0.4830143 1 24 207 -0.05317584 2 7 235 0.0619729 3 6 127 -0.3035713 4 5 -1 -0.0581057 -1 0.0118677 -1 0.0604271 141 -0.01617906 8 17 164 0.03152823 9 14 136 -0.1012658 10 11 -1 0.0514364 140 -0.5662499 12 13 -1 0.168355 -1 0.0753201 281 123 15 16"
   "-1 0.184804 -1 0.102467 120 0.1305677 18 19 -1 0.0051561 282 70 20 21 -1 0.17161 210 0.3828494 22 23 -1 0.011351 -1 0.164871 26 0.5983335 25 28 166 0.09841189 26 27 -1 -0.0481059 -1 0.103584 -1 -0.072679 29 24 0.6901972 1 16 47 0.04018167 2 11 24 0.5535391 3 8 237 -0.5796395 4 7 268 0.2844827 5 6 -1 -0.105859 -1 -0.0221887 -1 -0.117512 41 0.09090906 9 10 -1 -0.098665 -1 -0.0324528 25 0.1096933 12 13 -1 -0.107926 260 0.2548553 14 15 -1 -0.0147247 -1 0.071801 24 0.824437 17 26 41 0.25 18 21 271 0.15 19 20 -1 -0.0665348 -1 0.0267872 25 0.3463154 22 23 -1 -0.057648 282 15 24 25 -1 -0.0258912 -1 0.0747555 77 -0.4431934 27 28 -1 0.0393529 -1 0.100402 29 33 0.9971254 1 16 102 0.1909086 2 9 115 0.08674491 3 8 30 0.07532204 4 5 -1 -0.0637697 23 0.7550018 6 7 -1 0.0274962 -1 -0.0522494 -1 -0.0780385 162 0.03100121 10 15 98 -0.2374991 11 14 38 0.06779661 12 13 -1 -0.0262462 -1 0.105169 -1 0.00647148"
   "-1 -0.0353748 101 -0.2163544 17 24 38 0.06521739 18 19 -1 0.00149603 41 0.08333331 20 21 -1 0.171035 194 0.1553694 22 23 -1 0.0454491 -1 0.138196 26 0.9234137 25 28 38 0.09999999 26 27 -1 -0.0612132 -1 0.0854709 -1 -0.0574103 29 25 0.2223991 1 24 28 0.4920497 2 15 34 0.8076922 3 10 187 -0.1206467 4 9 25 0.0426321 5 6 -1 0.123344 281 155 7 8 -1 0.0838416 -1 -0.0442513 -1 0.15327 227 -0.2285713 11 12 -1 -0.0513591 281 115 13 14 -1 0.129005 -1 -0.0192592 260 0.1556043 16 21 36 0.5681817 17 18 -1 0.0990755 38 0.04 19 20 -1 0.0954647 -1 0.0226852 111 -0.9038459 22 23 -1 0.0746901 -1 -0.0275351 26 0.6122659 25 28 34 0.4999997 26 27 -1 0.0154038 -1 -0.0595485 -1 -0.0857053 29 25 0.5046553 1 24 208 0.01234435 2 15 73 -0.4472563 3 6 268 0.3787878 4 5 -1 -0.0507594 -1 0.0639608 189 -0.2037657 7 12 99 -0.732882 8 11 12 0.4999998 9 10 -1 0.0982369 -1 0.258818 -1 0.0454564 254 0.2679651 13 14 -1 -0.00881896"
   "-1 0.0870295 235 0.09565644 16 23 271 0.6749997 17 22 127 0.5999995 18 21 139 -0.009865948 19 20 -1 0.0629711 -1 -0.00152317 -1 0.122124 -1 0.138341 -1 0.13583 26 0.459954 25 28 215 0.309098 26 27 -1 -0.0301112 -1 0.145413 -1 -0.0690159 29 24 0.6745531 1 14 93 0.04531453 2 7 24 0.5228494 3 4 -1 -0.106705 26 0.9460563 5 6 -1 -0.0696547 -1 0.0284391 260 0.3823677 8 13 26 0.7134844 9 12 261 -0.6011373 10 11 -1 0.0340576 -1 -0.0909083 -1 0.0262891 -1 0.0509225 70 0.01002294 15 22 282 15 16 17 -1 -0.0821284 49 -0.03605271 18 19 -1 -0.00387966 33 0.999999 20 21 -1 0.0699646 -1 0.014786 25 0.3671716 23 24 -1 0.00209164 8 -0.423073 25 26 -1 0.0414687 26 0.8380342 27 28 -1 0.0480726 -1 0.0970474 29 33 0.9944435 1 14 102 0.1543109 2 7 115 0.08674491 3 6 33 0.9222661 4 5 -1 -0.0575351 -1 -0.014608 -1 -0.0761481 161 -0.07213179 8 13 38 0.06521739 9 10 -1 -0.0360332 41 0.04545454 11 12 -1 0.151644 -1"
   "0.0321107 -1 -0.0278805 115 0.01535474 15 26 38 0.1118568 16 21 281 162 17 20 280 0.9166667 18 19 -1 -0.0534979 -1 0.104379 -1 0.116834 40 0.4545454 22 23 -1 0.137259 282 15 24 25 -1 0.141348 -1 0.0415818 26 0.9460563 27 28 -1 0.0131523 -1 -0.0641748 29 25 0.2466331 1 24 34 0.5919002 2 13 128 -0.1923076 3 6 37 0.2777777 4 5 -1 0.131864 -1 0.0920944 115 0.05791778 7 12 145 -0.626834 8 9 -1 0.0357366 31 0.4828844 10 11 -1 0.1284 -1 0.0514591 -1 -0.00443227 25 0.08816359 14 21 120 0.1495178 15 18 202 0.02499998 16 17 -1 0.0424215 -1 0.105034 210 -0.04253468 19 20 -1 0.0661344 -1 -0.0247886 256 -0.05391135 22 23 -1 0.0488486 -1 -0.0317223 26 0.6122659 25 28 31 0.02317664 26 27 -1 0.224901 -1 -0.0431291 -1 -0.0846126 29 30 0.5005651 1 24 207 -0.2366515 2 7 235 0.1078282 3 6 127 -0.4819276 4 5 -1 -0.0664033 -1 -0.00327478 -1 0.0477846 140 -0.1221296 8 21 282 7 9 10 -1 -0.0199905 176 -0.7058823"
   "11 14 195 -1.247934 12 13 -1 0.146487 -1 0.0207196 235 0.09293157 15 20 76 -0.8741233 16 17 -1 0.0563871 25 0.2604601 18 19 -1 0.134557 -1 0.0225156 -1 0.144008 210 0.3208133 22 23 -1 0.0128196 -1 0.0939799 26 0.6358022 25 28 23 0.6754644 26 27 -1 -0.0364229 -1 0.144594 -1 -0.0691607 29 24 0.6955917 1 14 25 0.1096933 2 3 -1 -0.106416 41 0.1999998 4 7 248 -0.08333322 5 6 -1 -0.0540358 -1 -0.098236 93 0.04911518 8 13 26 0.6465874 9 10 -1 -0.0664254 282 15 11 12 -1 -0.0774278 -1 0.00991337 -1 0.0290771 24 0.824437 15 22 40 0.3333331 16 17 -1 -0.0316049 25 0.3273253 18 19 -1 -0.04984 280 0.07727273 20 21 -1 -0.0381833 -1 0.0598191 51 -0.2835617 23 26 36 0.1754386 24 25 -1 -0.0390011 -1 0.0596251 30 0.3847469 27 28 -1 0.0156446 -1 0.0918183 29 33 0.9888879 1 14 23 0.8653725 2 13 30 0.08637716 3 4 -1 -0.0544353 41 0.173913 5 8 30 0.3320477 6 7 -1 0.00429661 -1 0.108458 51 -0.3814701 9 10 -1 0.0336962"
   "26 0.8617145 11 12 -1 -0.000558734 -1 -0.0629141 -1 -0.0726452 115 0.01535474 15 26 30 0.08637716 16 19 34 0.8095237 17 18 -1 -0.0315049 -1 0.0811591 41 0.08333331 20 21 -1 0.130088 282 15 22 23 -1 0.126691 24 0.8314916 24 25 -1 0.0554167 -1 -0.0384695 26 0.8880365 27 28 -1 0.0257826 -1 -0.0533145 29 25 0.2604601 1 24 36 0.55 2 17 163 -0.03492787 3 14 34 0.3142857 4 11 80 -0.5192307 5 6 -1 0.126174 31 0.2803061 7 8 -1 0.111537 42 0.01960784 9 10 -1 -0.0838931 -1 0.0794159 141 -0.8927635 12 13 -1 0.0311986 -1 0.0906968 123 -0.06997248 15 16 -1 0.0654665 -1 0.00776806 25 0.06578656 18 23 38 0.06521739 19 22 4 0.3772116 20 21 -1 0.0849711 -1 0.0127663 -1 0.0104283 -1 -0.0123658 26 0.5210106 25 28 238 -0.9796591 26 27 -1 0.147167 -1 -0.0385311 -1 -0.0825761 29 25 0.5912395 1 28 161 -0.6037258 2 13 280 0.76 3 12 139 -0.853524 4 11 281 128 5 10 122 -0.9585515 6 7 -1 0.0177524 71 -1.022393 8 9 -1"
   "0.336788 -1 0.100463 -1 -0.0366943 -1 -0.0195472 -1 -0.0745758 140 -0.4250482 14 21 280 0.09734513 15 16 -1 0.000163775 214 -0.6793466 17 18 -1 0.0223011 84 -1.281553 19 20 -1 0.0590247 -1 0.120379 168 0.08046379 22 25 27 0.5321706 23 24 -1 -0.03716 -1 0.0224463 210 0.329035 26 27 -1 0.0462244 -1 0.121507 -1 -0.0638488 29 24 0.6745531 1 16 25 0.1096933 2 3 -1 -0.100868 93 0.0535031 4 13 40 0.09090907 5 6 -1 -0.0947913 282 206 7 12 26 0.9316518 8 11 231 -0.08546057 9 10 -1 -0.0288418 -1 -0.0737 -1 0.0259447 -1 0.0585436 268 0.361111 14 15 -1 -0.0237518 -1 0.0661966 70 0.01002294 17 22 282 14 18 19 -1 -0.073442 49 -0.02339415 20 21 -1 -0.00530015 -1 0.048282 26 0.8454525 23 26 256 0.01169596 24 25 -1 -0.0404159 -1 0.048097 54 -0.2733661 27 28 -1 0.0481229 -1 0.0873229 29 33 0.9836846 1 12 23 0.8653725 2 11 30 0.08637716 3 4 -1 -0.052243 26 0.8849329 5 8 256 0.1112526 6 7 -1 0.0433544 -1 -0.022199"
   "5 -0.3836661 9 10 -1 0.0229974 -1 -0.0639653 -1 -0.0704727 101 -0.1613759 13 24 240 0.01592386 14 19 282 15 15 16 -1 0.126918 281 16 17 18 -1 0.117444 -1 0.058306 7 0.1641499 20 21 -1 -0.00379527 132 -0.09999992 22 23 -1 0.113066 -1 0.022805 26 0.9234137 25 28 38 0.09999999 26 27 -1 -0.0542211 -1 0.0441535 -1 -0.058085 29 25 0.2604601 1 26 36 0.5853658 2 21 255 0.2219254 3 16 34 0.4528302 4 11 11 -0.5793103 5 6 -1 0.108991 42 0.03921568 7 10 31 0.1108186 8 9 -1 0.120786 -1 -0.069146 -1 0.0910204 141 -0.7426208 12 15 193 -0.1478772 13 14 -1 -0.0242129 -1 0.0623 -1 0.0773746 130 -0.5940594 17 18 -1 0.085954 54 -0.3600302 19 20 -1 0.0417372 -1 -0.0322461 38 0.04 22 25 178 0.09374995 23 24 -1 0.0727549 -1 0.00304711 -1 -0.0035938 26 0.6122659 27 28 -1 -0.0359205 -1 -0.0824852 29 25 0.5912395 1 24 161 -0.648792 2 9 235 0.1177075 3 8 203 -1.430555 4 5 -1 0.0860514 32 0.47979 6 7 -1 -0.0652822 -1"
   "-0.00772575 -1 0.0403846 140 -0.4250482 10 19 280 0.09734513 11 12 -1 -3.83573e-05 214 -0.6793466 13 14 -1 0.0171003 126 -3.787877 15 16 -1 -0.0389245 127 0.3749996 17 18 -1 0.082842 -1 0.124263 168 0.02995384 20 21 -1 0.00316863 210 0.329035 22 23 -1 0.038345 -1 0.103667 26 0.4287097 25 28 212 0.1056873 26 27 -1 -0.0558156 -1 0.123855 -1 -0.0638652 29 24 0.6745531 1 12 25 0.1096933 2 3 -1 -0.0961757 93 0.0535031 4 9 41 0.09090906 5 6 -1 -0.0829566 26 0.894296 7 8 -1 -0.0484342 -1 0.0248273 277 5.5 10 11 -1 -0.0249363 -1 0.0584488 24 0.824437 13 20 40 0.3333331 14 15 -1 -0.0338329 25 0.3463154 16 17 -1 -0.0415202 133 0 18 19 -1 0.0161066 -1 0.0614016 77 -0.4053046 21 24 36 0.1754386 22 23 -1 -0.0651765 -1 0.0426613 30 0.3847469 25 28 122 -0.4397547 26 27 -1 -0.137986 -1 0.0446411 -1 0.0818114 29 33 0.9810922 1 12 23 0.8755061 2 11 30 0.09177791 3 4 -1 -0.0496266 26 0.8617145 5 8 25 0.8022573"
   "6 7 -1 0.00257896 -1 0.0750517 8 -0.4957411 9 10 -1 0.0328656 -1 -0.054672 -1 -0.0689458 116 0.05343389 13 26 38 0.1118568 14 19 281 162 15 18 280 0.9130435 16 17 -1 -0.0459833 -1 0.074299 -1 0.0831431 41 0.08333331 20 21 -1 0.102829 282 15 22 23 -1 0.0989233 281 17 24 25 -1 0.0770575 -1 0.0134921 99 0.1843562 27 28 -1 -0.0524754 -1 0.0292259 29 25 0.2928597 1 22 28 0.6705495 2 17 102 -0.009667933 3 10 134 -0.5925924 4 5 -1 0.0986675 281 185 6 9 28 0.2705092 7 8 -1 0.0991635 -1 0.050066 -1 -0.0819565 34 0.7835051 11 14 132 0.1999998 12 13 -1 0.0634376 -1 -0.0172371 227 -0.1964285 15 16 -1 -0.0616819 -1 0.0184224 260 0.2548553 18 21 36 0.5714285 19 20 -1 0.0526523 -1 0.00759437 -1 -0.0369274 26 0.5210106 23 28 141 0.1927447 24 27 19 -3.473683 25 26 -1 0.284525 -1 -0.0517659 -1 0.0711632 -1 -0.0807871 29 25 0.5912395 1 22 207 -0.2756845 2 7 127 -0.4999998 3 4 -1 -0.051434 235 0.3007946 5 6"
   "-1 -0.000241451 -1 0.0723275 185 0.06393737 8 19 50 -0.2467897 9 12 123 0.03815121 10 11 -1 0.0167109 -1 0.0838108 26 0.8000851 13 18 281 74 14 15 -1 0.111734 198 -0.2857141 16 17 -1 0.0977118 -1 0.0449187 -1 -0.0355124 235 0.2328139 20 21 -1 0.00564852 -1 0.0611917 232 -0.2298431 23 28 40 1.374999 24 27 242 1.916665 25 26 -1 -0.0236562 -1 0.291259 -1 0.404398 -1 -0.0632574 29 26 0.8258392 1 18 93 0.0535031 2 13 25 0.173618 3 4 -1 -0.0916421 282 15 5 6 -1 -0.0936746 40 0.09090907 7 8 -1 -0.0772873 282 216 9 12 254 0.02003263 10 11 -1 -0.0540298 -1 -0.0046057 -1 0.0933118 268 0.3124997 14 17 253 -0.3796479 15 16 -1 0.033911 -1 -0.054102 -1 0.0490744 74 -0.3570012 19 26 282 10 20 21 -1 -0.0745455 40 0.1754385 22 23 -1 -0.0562206 186 -0.004091369 24 25 -1 0.0532305 -1 -0.00335006 280 0.1446541 27 28 -1 0.0131514 -1 0.0751881 29 33 0.9810922 1 8 23 0.8755061 2 7 30 0.07532204 3 4 -1 -0.0478266"
   "162 0.040106 5 6 -1 0.0167054 -1 -0.0341463 -1 -0.0660702 121 -0.2191764 9 18 38 0.03333333 10 11 -1 -0.0126611 41 0.08333331 12 13 -1 0.096831 282 15 14 15 -1 0.100018 281 17 16 17 -1 0.0798541 -1 0.0101332 26 0.9374975 19 24 38 0.09999999 20 21 -1 -0.0391907 158 0.04166665 22 23 -1 0.0654371 -1 0.00352277 51 -0.1671525 25 28 187 0.07385813 26 27 -1 -0.0417041 -1 0.0861953 -1 -0.0711885 29 25 0.2928597 1 22 28 0.4146214 2 11 281 161 3 8 33 0.9758055 4 5 -1 0.0913945 281 29 6 7 -1 -0.00935854 -1 0.0614632 88 -0.5079364 9 10 -1 0.0481476 -1 -0.0516249 255 0.3449015 12 19 280 0.8305085 13 18 209 -0.0100096 14 15 -1 0.0581233 129 0.4886363 16 17 -1 0.0313063 -1 -0.0269785 -1 -0.0416902 111 -0.9038459 20 21 -1 0.0511962 -1 -0.037672 26 0.5210106 23 28 34 0.5121951 24 27 139 0.5829673 25 26 -1 0.00981606 -1 0.232262 -1 -0.0535488 -1 -0.0796886 29 30 0.6194945 1 24 210 0.0001952098 2 17 170 -0.4039083"
   "3 4 -1 -0.039193 140 -0.7476461 5 14 168 -1.00189 6 7 -1 -0.0186385 280 0.08928571 8 9 -1 -0.0330591 192 -0.3137189 10 11 -1 0.136975 214 -0.8140028 12 13 -1 -0.0246329 -1 0.0922882 255 0.3916062 15 16 -1 -0.016435 -1 0.0499508 156 0.1874998 18 23 179 0.2272726 19 22 129 0.2954544 20 21 -1 0.0271939 -1 0.0822341 -1 0.0848968 -1 0.0201041 26 0.8304324 25 28 33 0.9661101 26 27 -1 0.0446042 -1 -0.0533327 -1 -0.0665277 29 26 0.821072 1 12 25 0.1096933 2 3 -1 -0.0890583 231 -0.09220785 4 9 5 -0.2263347 5 6 -1 -0.0426832 277 5.5 7 8 -1 -0.00295553 -1 0.0602796 24 0.5228494 10 11 -1 -0.0807464 -1 -0.0432318 5 -0.2437398 13 22 121 -0.4042529 14 19 282 11 15 16 -1 -0.0852503 195 0.0595238 17 18 -1 0.0217498 -1 -0.0422194 26 0.9090723 20 21 -1 -0.022679 -1 0.0429875 41 0.1999998 23 26 271 0.1249999 24 25 -1 -0.045013 -1 0.0534965 281 241 27 28 -1 0.0738481 -1 -0.250398 29 102 0.1543109 1 16 116 0.1087122"
   "2 11 33 0.9222661 3 4 -1 -0.044866 263 -0.01925987 5 8 26 0.07504753 6 7 -1 -0.060003 -1 -0.00297092 129 -0.2307692 9 10 -1 0.0640729 -1 0.002415 217 -347.2222 12 15 123 0.10154 13 14 -1 0.332583 -1 -0.0630469 -1 -0.0641946 33 0.9805546 17 20 280 0.1011905 18 19 -1 0.0614212 -1 -0.0098012 240 0.009969269 21 26 112 0.03571428 22 25 99 0.2511643 23 24 -1 0.0693363 -1 0.106785 -1 0.0104419 145 0.232837 27 28 -1 0.00766239 -1 0.0608468 29 25 0.2928597 1 22 36 0.6010928 2 13 88 -0.6363636 3 6 34 0.304054 4 5 -1 0.0865581 -1 0.042628 255 0.2521934 7 12 187 0.04471266 8 11 281 164 9 10 -1 0.0366513 -1 -0.0526408 -1 0.0736121 -1 -0.00551628 38 0.06521739 14 17 26 0.1545713 15 16 -1 0.0470325 -1 -0.011364 24 0.4379727 18 19 -1 -0.0275492 28 0.7677344 20 21 -1 0.105392 -1 -0.00529196 26 0.6122659 23 28 34 0.6999999 24 27 141 0.2015537 25 26 -1 -0.0201887 -1 0.10644 -1 -0.0656506 -1 -0.0799179 29 30"
   "0.6286951 1 22 235 0.09873972 2 17 254 0.629646 3 14 225 0.08749997 4 7 127 -0.4819276 5 6 -1 -0.0579268 -1 -0.00912026 156 0 8 13 32 0.4377554 9 10 -1 0.00268101 24 0.2770544 11 12 -1 0.0825257 -1 0.0157864 -1 -0.0095602 232 -0.6897076 15 16 -1 -0.0106773 -1 0.0769974 129 0.4499998 18 21 163 0.03970461 19 20 -1 0.0303293 -1 0.0710822 -1 0.086934 235 0.2212113 23 24 -1 -0.062569 260 -0.4321573 25 28 53 1.319178 26 27 -1 0.0618452 -1 0.425258 -1 -0.0387668 29 26 0.8163603 1 18 25 0.1096933 2 3 -1 -0.0857751 231 -0.09220785 4 11 260 0.2447323 5 8 253 -0.2131301 6 7 -1 0.0137746 -1 -0.0479689 155 -0.2499999 9 10 -1 -0.0488042 -1 0.0509239 40 0.09090907 12 13 -1 -0.0860761 26 0.5851879 14 15 -1 -0.0700506 282 221 16 17 -1 -0.0316263 -1 0.135927 74 -0.2451026 19 26 186 -0.06814868 20 23 40 0.1754385 21 22 -1 -0.0535332 -1 0.0543645 282 15 24 25 -1 -0.0562965 -1 -0.000438942 41 0.1578947 27 28"
   "-1 0.0121658 -1 0.0693315 29 33 0.9800756 1 10 23 0.8755061 2 7 38 0.07444168 3 4 -1 -0.0485588 164 0.0065232 5 6 -1 0.0139183 -1 -0.0310227 28 0.5195051 8 9 -1 0.112311 -1 -0.063859 135 0.02941175 11 22 240 0.01592386 12 17 194 0.1085878 13 16 40 0.2857141 14 15 -1 0.0731189 -1 0.0107034 -1 0.0832744 7 0.1641499 18 19 -1 -0.00266078 132 -0.09090906 20 21 -1 0.0863579 -1 0.0126275 24 0.8629343 23 28 38 0.03773585 24 25 -1 -0.0623727 41 0.1999998 26 27 -1 0.0489559 -1 -0.0101564 -1 -0.0636156 29 25 0.4180715 1 26 37 0.4580152 2 15 33 0.9765182 3 10 255 0.4729258 4 9 13 -0.173913 5 6 -1 0.0853565 187 -0.8627819 7 8 -1 -0.0241882 -1 0.0589315 -1 0.0150999 240 0.1732934 11 14 65 -0.4666665 12 13 -1 0.0356647 -1 -0.0352867 -1 0.0466023 280 0.6153846 16 23 282 127 17 22 25 0.08282205 18 21 17 -0.5999998 19 20 -1 0.0638408 -1 0.0168017 -1 -0.0109869 -1 0.109807 111 -1.53211 24 25 -1 0.0619464 -1"
   "-0.0303447 24 0.3184875 27 28 -1 0.0211338 -1 -0.0778706 29 25 0.6437909 1 24 235 0.1177075 2 15 254 0.629646 3 12 215 -0.3649958 4 5 -1 -0.0383935 195 -1.468749 6 7 -1 0.0950215 209 0.03756013 8 11 141 -0.708183 9 10 -1 0.0205832 -1 -0.026565 -1 0.0255058 232 -0.6897076 13 14 -1 -0.00885362 -1 0.068712 129 0.03124998 16 19 113 0.1979166 17 18 -1 0.016742 -1 0.0697924 26 0.6465874 20 23 281 108 21 22 -1 0.0865906 -1 0.0280453 -1 0.00661312 235 0.2212113 25 26 -1 -0.0607224 255 -0.5498321 27 28 -1 0.103124 -1 -0.0322937 29 26 0.8258392 1 16 25 0.1096933 2 3 -1 -0.0828244 231 -0.09220785 4 11 8 -0.3628625 5 6 -1 -0.0418619 260 0.2447323 7 10 277 2.5 8 9 -1 -0.0397915 -1 0.0217163 -1 0.0523032 24 0.5228494 12 13 -1 -0.0728658 282 15 14 15 -1 -0.0786389 -1 -0.0228425 123 -0.2176516 17 24 282 8 18 19 -1 -0.0701603 40 0.1075269 20 21 -1 -0.055385 99 0.2436801 22 23 -1 0.0367408 -1 -0.00890654 26"
   "0.942917 25 28 41 0.1999998 26 27 -1 -0.0160523 -1 0.0512809 -1 0.0718824 29 102 -0.003957673 1 8 116 0.1391364 2 7 117 0.1256258 3 6 281 150 4 5 -1 -0.0357539 -1 0.0269658 -1 0.0339135 -1 -0.0641962 280 0.1011905 9 14 281 101 10 11 -1 -0.0160131 72 0.02057735 12 13 -1 0.0901026 -1 0.0293987 281 17 15 18 101 -0.1325532 16 17 -1 0.0657069 -1 -0.0218301 26 0.941153 19 26 30 0.09177791 20 21 -1 -0.0323492 40 0.04854368 22 23 -1 0.0844475 104 0.4166661 24 25 -1 0.0249494 -1 -0.0378222 28 0.3558098 27 28 -1 0.034373 -1 -0.0468187 29 25 0.4180715 1 26 28 0.2705092 2 5 281 129 3 4 -1 0.0744685 -1 0.0130532 255 0.2639714 6 23 280 0.8305085 7 20 187 0.04717404 8 17 127 0.5277775 9 16 19 -0.4166665 10 11 -1 0.0534087 281 91 12 15 96 -0.4448047 13 14 -1 0.0999069 -1 0.0149767 -1 -0.018715 -1 -0.014854 5 0.009179704 18 19 -1 0.0795517 -1 0.0284624 172 -0.8749994 21 22 -1 0.0274069 -1 -0.0634684 48 -1.055118"
   "24 25 -1 0.029844 -1 -0.037429 24 0.3184875 27 28 -1 0.0204617 -1 -0.0768002 29 30 0.774277 1 24 235 0.04078773 2 15 256 0.7793506 3 12 121 -0.9615296 4 5 -1 -0.0547482 139 -0.5055161 6 9 281 147 7 8 -1 0.0437919 -1 -0.034867 210 0.1966396 10 11 -1 -0.0283188 -1 0.0193168 237 -0.8450614 13 14 -1 -0.00602914 -1 0.0681131 35 0.9999997 16 23 26 0.8454525 17 20 210 0.05081796 18 19 -1 0.0221678 -1 0.0548303 40 1.666666 21 22 -1 -0.0301143 -1 0.405927 -1 0.0770656 53 2.042717 25 26 -1 -0.0597321 208 0.03183851 27 28 -1 -0.0461221 -1 0.303543 29 26 0.8380342 1 20 24 0.2664755 2 5 231 -0.582323 3 4 -1 -0.0442734 -1 -0.084876 233 -0.1576008 6 11 260 0.4798079 7 10 253 -0.2131301 8 9 -1 0.0213212 -1 -0.032361 -1 0.0509234 25 0.3273253 12 13 -1 -0.0786872 40 0.09090907 14 15 -1 -0.07275 282 221 16 19 254 0.01591231 17 18 -1 -0.0477279 -1 0.00795055 -1 0.114359 74 -0.3835348 21 24 48 0.273887 22 23"
   "-1 0.0180231 -1 -0.0294449 280 0.1563981 25 26 -1 0.00933187 41 0.2499999 27 28 -1 0.0262941 -1 0.0659365 29 102 -0.009667933 1 14 116 0.1391364 2 13 30 0.07532204 3 10 13 -0.3333333 4 5 -1 -0.0690687 44 0 6 9 251 -0.04545453 7 8 -1 -0.0206142 -1 0.333386 -1 -0.0256448 41 0.1999998 11 12 -1 0.043958 -1 -0.0145125 -1 -0.061702 98 -0.2374991 15 24 282 15 16 19 281 103 17 18 -1 -0.00931326 -1 0.084036 53 0.1750143 20 21 -1 0.0127591 40 0.4516128 22 23 -1 0.0768632 -1 0.0218522 43 0.6399999 25 28 33 0.999999 26 27 -1 -0.00208209 -1 0.0388364 -1 -0.0273467 29 25 0.4842724 1 24 36 0.5853658 2 19 88 -0.6363636 3 8 34 0.304054 4 5 -1 0.0719351 196 -0.340909 6 7 -1 -0.0131378 -1 0.0467825 42 0.02840909 9 12 29 0.2457172 10 11 -1 0.0789062 -1 -0.0866784 185 0.2891221 13 18 25 0.07137809 14 15 -1 0.0386421 39 0.02777777 16 17 -1 -0.0362049 -1 0.0191731 -1 0.0725817 25 0.08816359 20 23 55 -0.6222042"
   "21 22 -1 0.0511029 -1 0.00454873 -1 -0.0255947 26 0.3987889 25 28 117 1.396029 26 27 -1 -0.00582106 -1 0.296276 -1 -0.0771126 29 30 0.774277 1 24 235 0.1393426 2 19 208 -0.3162779 3 6 203 -1.521739 4 5 -1 0.0978529 -1 -0.0280647 25 0.2756787 7 18 130 0.04166666 8 17 199 -0.2287736 9 12 282 7 10 11 -1 -0.0356907 -1 0.0462057 212 -1.741505 13 14 -1 0.332321 168 0.03998098 15 16 -1 -0.0174182 -1 0.0296576 -1 0.0676107 -1 -0.0193937 197 -0.9693251 20 21 -1 0.083831 163 0.0653685 22 23 -1 0.0259418 -1 0.0588162 270 1.249999 25 26 -1 -0.0577914 249 0.4761903 27 28 -1 -0.00243504 -1 0.285533 29 26 0.8059958 1 14 25 0.1096933 2 3 -1 -0.07767 233 -0.2639627 4 9 277 2.5 5 6 -1 -0.036957 63 -0.4999997 7 8 -1 -0.0434967 -1 0.0425262 282 201 10 13 41 0.1249999 11 12 -1 -0.0723842 -1 -0.0321615 -1 0.0389951 5 -0.2437398 15 22 186 -0.09131889 16 17 -1 0.0360272 26 0.9557858 18 19 -1 -0.0336599 8 -1.256511"
   "20 21 -1 -0.119066 -1 0.0168078 133 0.05999999 23 28 38 0.6071428 24 25 -1 -0.041459 41 0.3333329 26 27 -1 -0.00455568 -1 0.0533011 -1 0.0639002 29 56 -0.0003469759 1 6 115 0.08674491 2 5 19 -1.461538 3 4 -1 -0.0667594 -1 -0.00992308 -1 -0.0566235 43 0.09999998 7 12 33 0.9973108 8 9 -1 0.0138658 273 0.1346153 10 11 -1 0.0838766 -1 0.0371991 26 0.9604387 13 24 25 0.9335964 14 23 38 0.07444168 15 18 42 0 16 17 -1 0.118443 -1 -0.0412228 39 0 19 22 269 0.1499999 20 21 -1 0.0756086 -1 0.0060652 -1 -0.00257025 -1 0.048811 82 -0.2499999 25 28 187 -0.02438401 26 27 -1 -0.0332754 -1 0.0636464 -1 -0.0538981 29 25 0.4842724 1 26 37 0.4460966 2 19 34 0.4799999 3 12 134 -0.2258063 4 7 198 -0.4999994 5 6 -1 0.0373783 -1 0.0769045 42 0.02840909 8 11 28 0.1307447 9 10 -1 0.0791981 -1 -0.0750502 -1 0.0426957 187 0.4033532 13 18 229 -0.4249998 14 15 -1 -0.0371195 77 -0.8730222 16 17 -1 0.0619663 -1 0.00513074"
   "-1 0.09364 280 0.6153846 20 25 282 127 21 24 77 0.1869965 22 23 -1 0.0231023 -1 -0.0154906 -1 0.092868 -1 -0.0246434 26 0.3987889 27 28 -1 0.013482 -1 -0.0761406 29 30 0.774277 1 24 140 -0.05202518 2 19 121 -0.9890058 3 4 -1 -0.0347235 184 0.301957 5 18 281 146 6 17 26 0.2892544 7 12 172 -3.540539 8 9 -1 -0.0631732 282 7 10 11 -1 -0.00632994 -1 0.0639763 177 0.02777777 13 14 -1 0.0584621 164 0.1242944 15 16 -1 -0.0148436 -1 0.0449179 -1 -0.0108188 -1 -0.00414171 27 0.4894632 20 21 -1 -0.0384584 282 71 22 23 -1 0.0252967 -1 -0.018371 26 0.754983 25 28 178 0.2499999 26 27 -1 -0.0496516 -1 0.0917919 -1 -0.0620093 29 26 0.8258392 1 18 25 0.1096933 2 5 232 -0.4547707 3 4 -1 -0.039312 -1 -0.0836983 282 15 6 7 -1 -0.0753421 40 0.09090907 8 9 -1 -0.0513073 255 0.06550841 10 13 282 206 11 12 -1 -0.024263 -1 0.062549 232 -0.08078976 14 15 -1 0.0471305 26 0.5722864 16 17 -1 -0.0626424 -1 0.0262269 51"
   "-0.2166576 19 24 186 -0.004091369 20 23 40 0.09999996 21 22 -1 -0.0714092 -1 0.0414962 -1 -0.0147751 26 0.9520156 25 28 41 0.1999998 26 27 -1 -0.0262407 -1 0.0430419 -1 0.0642395 29 102 -0.009667933 1 10 116 0.1391364 2 9 71 0.1229626 3 8 281 150 4 7 184 0.8103534 5 6 -1 -0.0337896 -1 0.0776678 -1 0.0305691 -1 0.0286419 -1 -0.0567731 282 15 11 14 281 98 12 13 -1 -0.0126019 -1 0.0717886 281 17 15 18 282 101 16 17 -1 -0.0220045 -1 0.0538864 41 0.04545454 19 24 30 0.02365925 20 21 -1 -0.0514144 271 0.05263156 22 23 -1 0.0834747 -1 0.0115853 216 1.574578 25 28 127 0.2499998 26 27 -1 1.80229e-05 -1 -0.0352343 -1 0.295004 29 25 0.4842724 1 26 36 0.6458333 2 19 88 -0.4999998 3 8 34 0.3142857 4 5 -1 0.0615731 149 -0.8928566 6 7 -1 -0.0172312 -1 0.0400622 42 0.02840909 9 12 29 0.2457172 10 11 -1 0.0708248 -1 -0.0803335 115 0.2490118 13 18 227 -0.2285713 14 15 -1 -0.00874109 50 -0.4864543 16 17 -1"
   "0.0654146 -1 0.027063 -1 -0.0304209 38 0.06521739 20 23 26 0.1545713 21 22 -1 0.0321014 -1 -0.0199035 243 0.4666665 24 25 -1 -0.0374772 -1 0.0168829 26 0.4287097 27 28 -1 0.0072491 -1 -0.0754495 29 30 0.774277 1 24 238 0.08563418 2 17 282 151 3 16 26 0.8419133 4 15 281 134 5 10 184 0.2805832 6 9 127 -0.5999998 7 8 -1 -0.012657 -1 0.0394116 -1 -0.011389 192 -1.390306 11 14 194 0.1317445 12 13 -1 0.395299 -1 -0.014211 -1 -0.0279736 -1 -0.0504785 -1 -0.0467915 35 1.05 18 23 261 0.04757207 19 22 255 0.3449015 20 21 -1 0.00328193 -1 0.0477389 -1 0.0498542 -1 0.0721192 98 1.042853 25 26 -1 -0.0533764 38 1.019608 27 28 -1 0.00206381 -1 0.396968 29 24 0.5604509 1 10 233 -0.4303372 2 7 277 5.5 3 6 268 0.2844827 4 5 -1 -0.054461 -1 0.0141424 -1 0.0373303 30 0.1259384 8 9 -1 -0.0813657 -1 -0.0466821 26 0.9115302 11 18 282 15 12 13 -1 -0.0594362 254 0.04174955 14 17 231 -0.02568578 15 16 -1 0.0130581"
   "-1 -0.0307356 -1 0.029463 74 -0.6405073 19 20 -1 -0.0220993 232 -0.4547707 21 22 -1 -0.0441373 54 -0.05320125 23 28 187 -0.110313 24 25 -1 0.0610011 206 -1.266666 26 27 -1 -0.207032 -1 0.0183093 -1 0.0627749 29 56 -0.005760325 1 6 162 0.040106 2 5 19 -1.461538 3 4 -1 -0.0643211 -1 -0.00510176 -1 -0.0491718 43 0.09999998 7 14 133 0.1833333 8 11 32 0.03929301 9 10 -1 -0.0587181 -1 0.0608916 280 0.9130435 12 13 -1 -0.0237112 -1 0.122077 26 0.9645399 15 26 25 0.9414524 16 25 281 139 17 20 254 0.05048877 18 19 -1 0.00587559 -1 -0.03498 47 -1.476808 21 22 -1 -0.0753871 72 -0.02986431 23 24 -1 0.0606892 -1 -0.0199429 -1 0.0407872 82 -0.2499999 27 28 -1 0.0168555 -1 -0.0506213 29 25 0.5199957 1 26 28 0.7444878 2 21 202 0.04545453 3 14 33 0.7486546 4 7 260 0.1398747 5 6 -1 0.0684561 -1 -0.00120154 42 0 8 9 -1 -0.0860918 34 0.4452054 10 11 -1 0.026702 142 -1.27365 12 13 -1 0.065059 -1 -0.0169004 31"
   "0.2404149 15 16 -1 0.0712915 281 19 17 18 -1 -0.0335375 120 0.1246746 19 20 -1 0.0535795 -1 0.00690599 38 0.2363636 22 25 24 0.9055946 23 24 -1 -0.00056207 -1 0.133697 -1 -0.0444623 19 -3.909089 27 28 -1 0.258827 -1 -0.0716873 29 30 0.774277 1 24 140 -0.2051089 2 17 214 -0.8140028 3 6 192 -0.8570977 4 5 -1 0.0781161 -1 -0.0289567 127 0.4615383 7 14 281 146 8 9 -1 0.0316184 192 -1.46146 10 13 199 -0.9266304 11 12 -1 0.361168 -1 -0.0709411 -1 -0.0229562 133 -0.8571419 15 16 -1 -0.0540194 -1 0.0647707 161 0.3764112 18 23 32 0.4421186 19 20 -1 -0.040188 282 71 21 22 -1 0.0151832 -1 -0.0229975 -1 0.0333682 260 -0.4151811 25 28 53 2.042717 26 27 -1 -0.0200031 -1 0.196192 -1 -0.0582198 29 24 0.5228494 1 8 233 -0.5787839 2 7 277 2.5 3 4 -1 -0.049246 55 -0.5539833 5 6 -1 -0.043733 -1 0.0405676 -1 -0.0660976 26 0.9445929 9 20 282 15 10 11 -1 -0.0530146 40 0.08333331 12 13 -1 -0.0421548 25 0.2011267"
   "14 15 -1 -0.0455593 97 0.3321236 16 19 187 0.022862 17 18 -1 0.024983 -1 -0.0115233 -1 0.0450112 51 -0.1834898 21 28 187 -0.4345457 22 23 -1 0.0550306 4 0.1885164 24 27 206 -1.359374 25 26 -1 -0.118455 -1 0.0110128 -1 -0.132591 -1 0.0581282 29 79 -0.0006030677 1 10 100 0.03842679 2 5 61 -0.117647 3 4 -1 -0.0266683 -1 0.0227829 48 -0.7541765 6 9 163 -0.9014287 7 8 -1 0.045336 -1 -0.0358169 -1 -0.0538763 41 0.03039513 11 20 38 0.03773585 12 15 25 0.2604601 13 14 -1 -0.0294066 -1 0.17472 271 0.04166665 16 19 282 198 17 18 -1 0.0881748 -1 -0.0043823 -1 0.0238299 173 0.02499999 21 24 191 -0.0003834232 22 23 -1 -0.00326382 -1 0.0328058 187 0.07967713 25 26 -1 -0.0250882 79 0 27 28 -1 0.117504 -1 -0.00320202 29 25 0.5199957 1 26 59 -0.2105263 2 9 33 0.9210136 3 6 60 -0.2222221 4 5 -1 0.0610986 -1 0.0207442 240 0.167696 7 8 -1 -0.00952345 -1 0.0362022 238 -0.5043467 10 13 268 0.7499999 11 12 -1 0.0581313"
   "-1 -0.0396737 280 0.7247191 14 25 101 -0.4257971 15 20 187 -0.7284695 16 19 38 0.0576923 17 18 -1 0.0287478 -1 -0.0671487 -1 0.0570354 202 0.02499998 21 22 -1 -0.0281167 134 0.7083333 23 24 -1 -0.00486751 -1 0.0487161 -1 -0.0478676 24 0.3184875 27 28 -1 0.032083 -1 -0.0730297 29 30 0.7987234 1 26 282 159 2 23 197 -0.827586 3 8 26 0.2783841 4 5 -1 0.0683805 127 0.752688 6 7 -1 -0.010946 -1 0.113028 255 0.2219254 9 20 218 -0.4903845 10 13 27 1.111161 11 12 -1 -0.0342537 -1 0.177421 127 1.490196 14 19 146 -0.2340135 15 18 50 -0.4864543 16 17 -1 -0.0121275 -1 0.0389531 -1 -0.00514773 -1 0.0766082 281 131 21 22 -1 0.0424745 -1 -0.00950632 199 -1.118055 24 25 -1 0.0623322 -1 -0.0483249 270 1.249999 27 28 -1 -0.0502069 -1 0.0802653 29 24 0.4981952 1 6 233 -0.5787839 2 5 277 5.5 3 4 -1 -0.0309514 -1 0.0390447 -1 -0.0658976 47 0.01432318 7 16 40 0.03846153 8 9 -1 -0.055892 38 0.7916666 10 13 110 -0.6166665"
   "11 12 -1 0.0655859 -1 -0.0378593 131 0.05769229 14 15 -1 -0.00581352 -1 0.0299545 26 0.9115302 17 20 146 0.04416707 18 19 -1 -0.0198039 -1 0.0275963 120 -0.4760203 21 24 13 -0.8169013 22 23 -1 -0.146451 -1 0.00661808 281 226 25 26 -1 0.0550809 186 0.1376984 27 28 -1 -0.53763 -1 0.0495674 29 142 -0.02265946 1 18 212 0.2642337 2 17 65 -0.6666664 3 6 38 0.03333333 4 5 -1 -0.0642457 -1 0.0121603 280 0.05714286 7 10 281 100 8 9 -1 -0.0190451 -1 0.0864015 40 0.03125 11 14 23 0.0550989 12 13 -1 -0.0635325 -1 0.0663594 281 10 15 16 -1 0.0528093 -1 0.00959328 -1 -0.0219065 24 0.9164938 19 26 33 0.91017 20 21 -1 -0.0406197 179 0.07377048 22 25 23 0.3099412 23 24 -1 -0.0269718 -1 0.0294889 -1 -0.0242391 77 -0.1829434 27 28 -1 0.0507695 -1 -0.0612419 29 30 0.5005651 1 26 37 0.4615384 2 15 33 0.9195331 3 8 128 -0.2068965 4 5 -1 0.059417 187 -0.8875519 6 7 -1 -0.0375321 -1 0.0326571 42 0 9 10 -1 -0.080457"
   "34 0.3636363 11 12 -1 0.0414964 224 -0.5714282 13 14 -1 -0.0460066 -1 0.0129517 280 0.6153846 16 23 282 127 17 22 63 -0.5999998 18 19 -1 0.0307678 74 0.0257323 20 21 -1 0.0122228 -1 -0.025777 -1 0.0779612 53 -0.9355333 24 25 -1 0.0105224 -1 -0.0392159 24 0.3184875 27 28 -1 0.0330275 -1 -0.0720951 29 30 0.7987234 1 24 98 0.6619189 2 19 282 144 3 14 28 1.048378 4 13 184 0.2805832 5 12 127 0.6249988 6 11 26 0.2783841 7 10 175 0.03571427 8 9 -1 0.00141429 -1 0.0356429 -1 -0.00839704 -1 0.045323 -1 -0.016575 -1 0.0732541 201 -0.5178568 15 18 220 -0.26 16 17 -1 -0.0723968 -1 0.103888 -1 -0.0496453 197 -0.9295774 20 21 -1 0.0879254 281 105 22 23 -1 0.0407404 -1 -0.012583 260 -0.4005976 25 28 231 0.2181087 26 27 -1 -0.022522 -1 0.11886 -1 -0.0558413 29 25 0.2011267 1 12 237 -0.4393043 2 7 277 5.5 3 4 -1 -0.0382858 138 -0.1584262 5 6 -1 -0.0688719 -1 0.0644874 280 0.8148148 8 9 -1 -0.0785723 24 0.2770544"
   "10 11 -1 -0.0599793 -1 0.0656545 41 0.1999998 13 16 248 -0.02678571 14 15 -1 -0.000811523 -1 -0.0512645 51 -0.2166576 17 24 13 -0.8333328 18 21 139 0.004754832 19 20 -1 -0.0168814 -1 -0.124509 187 -0.6557048 22 23 -1 0.0433026 -1 -0.000813547 26 0.8520972 25 26 -1 0.0140077 281 241 27 28 -1 0.0503082 -1 -0.255031 29 280 0.1171171 1 10 281 101 2 5 0 -0.4183567 3 4 -1 -0.0722051 -1 0.0426965 282 7 6 9 157 -0.6666665 7 8 -1 0.00246916 -1 0.0950257 -1 0.0308092 280 0.9090909 11 22 24 0.9271627 12 21 38 0.07444168 13 14 -1 -0.0421177 39 0.09999996 15 18 269 0.07812498 16 17 -1 0.05042 -1 0.00287267 176 -0.2083333 19 20 -1 0.0091941 -1 -0.0221169 -1 -0.048231 31 0.02317664 23 24 -1 -0.0777802 282 102 25 26 -1 -0.0406428 34 0 27 28 -1 0.123792 -1 0.0354128 29 30 0.563355 1 28 36 0.5964912 2 19 185 0.3070837 3 18 32 0.03929301 4 5 -1 0.0718911 42 0 6 9 65 -0.9090906 7 8 -1 0.0557998 -1 -0.0850896"
   "48 -1.475249 10 11 -1 0.0539498 25 0.1096933 12 17 200 62500 13 16 145 -0.7492724 14 15 -1 0.00617056 -1 0.0378427 -1 -0.0892455 -1 -0.011916 -1 0.0567805 38 0.1929824 20 25 134 0.5523809 21 24 1 -1.86478 22 23 -1 0.0636749 -1 -0.0125889 -1 0.0376298 33 0.7486546 26 27 -1 0.0538259 -1 -0.0408617 -1 -0.0681253 29 30 0.7987234 1 20 145 -0.4062808 2 11 214 -0.9639091 3 4 -1 -0.0334539 281 146 5 6 -1 0.0354419 192 -1.278525 7 10 194 0 8 9 -1 0.336135 -1 -0.0219164 -1 -0.0246111 210 0.3208133 12 19 260 0.1917501 13 16 268 -0.1160714 14 15 -1 0.0127624 -1 -0.0307556 33 0.9413164 17 18 -1 0.0356831 -1 -0.0182897 -1 0.0300226 33 0.9594946 21 28 123 1.265574 22 27 26 0.7937791 23 26 270 0.2142856 24 25 -1 -0.00765713 -1 0.173893 -1 -0.033084 -1 0.363016 -1 -0.0558215 29 25 0.2011267 1 8 237 -0.4393043 2 5 277 5.5 3 4 -1 -0.0355041 -1 0.037976 282 233 6 7 -1 -0.0724209 -1 0.0784137 5 -0.3918797 9 18"
   "40 0.1428571 10 11 -1 -0.0524494 4 0.1671893 12 17 185 0.01027386 13 16 282 8 14 15 -1 -0.0502779 -1 0.0234854 -1 -0.0304143 -1 -0.137533 26 0.9115302 19 24 254 0.02127142 20 23 49 0.01740105 21 22 -1 -0.0282872 -1 0.0138026 -1 0.0239066 232 -0.4547707 25 26 -1 -0.0666389 281 241 27 28 -1 0.0473031 -1 -0.198715 29 162 0.05690443 1 24 204 0 2 19 195 -0.1730768 3 14 107 -1.111111 4 5 -1 -0.0641249 25 0.03949014 6 7 -1 -0.0340281 26 0.9557858 8 11 11 -0.5454543 9 10 -1 0.0688728 -1 0.0183535 48 1.198395 12 13 -1 -0.0456043 -1 0.172465 142 0.005976543 15 18 31 0.02317664 16 17 -1 -0.0806503 -1 0.0462669 -1 -0.00162836 185 0.07618243 20 21 -1 -0.0266817 71 0.3551797 22 23 -1 -0.0011011 -1 0.0525953 186 0.05657612 25 28 282 241 26 27 -1 -0.0365341 -1 0.290372 -1 0.00316001 29 30 0.5868581 1 26 59 -0.2105263 2 11 33 0.9210136 3 8 37 0.7499999 4 7 163 0.03430739 5 6 -1 0.0538618 -1 0.0152513 -1 -0.00310422"
   "240 0.1294269 9 10 -1 -0.0117103 -1 0.0286364 238 -0.7178113 12 15 115 -0.06544111 13 14 -1 0.0649336 -1 -0.0471109 280 0.7247191 16 23 101 -0.2485164 17 22 182 0.2307692 18 21 191 -0.7734097 19 20 -1 -0.046854 -1 0.02017 -1 0.0506052 -1 -0.0112744 99 -0.901134 24 25 -1 0.0204983 -1 -0.0556527 19 -3.473683 27 28 -1 0.208595 -1 -0.0686517 29 30 0.8175334 1 20 198 -0.2016129 2 9 55 -0.2047095 3 8 203 -3.068181 4 5 -1 0.128017 226 -0.3095237 6 7 -1 -0.0425186 -1 0.00803429 -1 0.045477 281 89 10 17 163 0.1481154 11 16 139 -0.347513 12 13 -1 0.0239183 151 -0.6923075 14 15 -1 0.103151 -1 -0.0186626 -1 0.0419646 266 0.431818 18 19 -1 -0.0255672 -1 0.0180667 267 0.9531248 21 28 1 1.427387 22 25 98 1.224847 23 24 -1 -0.0491194 -1 0.134493 202 -0.07142855 26 27 -1 0.603058 -1 -0.0455164 -1 0.0777589 29 24 0.2979108 1 8 233 -0.5266291 2 7 261 -0.6011373 3 4 -1 0.0635023 268 0.1944444 5 6 -1 -0.0622871"
   "-1 0.00678214 -1 -0.0698249 26 0.9475663 9 20 282 15 10 11 -1 -0.0525023 13 -0.499999 12 13 -1 -0.0221393 255 0.09026668 14 19 230 0.07444038 15 18 23 0.8363763 16 17 -1 0.00103421 -1 0.0458504 -1 -0.0245793 -1 0.0306105 54 -0.05320125 21 28 201 -0.215909 22 23 -1 0.0458059 36 0.2577319 24 25 -1 -0.0450865 38 0.7106598 26 27 -1 -0.0835662 -1 0.0227596 -1 0.0503077 29 282 15 1 10 281 98 2 5 38 0.2713178 3 4 -1 -0.0726118 -1 0.0172945 203 -0.6818181 6 7 -1 0.00127488 33 0.8262573 8 9 -1 0.190009 -1 0.0610117 162 0.02271379 11 22 38 0.05555555 12 17 282 113 13 14 -1 -0.0495076 19 -0.6923075 15 16 -1 -0.0410605 -1 0.0732418 39 0.1111111 18 21 269 0.1249999 19 20 -1 0.042148 -1 0.00381055 -1 -0.000312154 282 233 23 28 202 0.678571 24 27 104 0.3437499 25 26 -1 -0.0195745 -1 -0.0535872 -1 0.0244432 -1 0.206846 29 30 0.5868581 1 26 28 0.8564267 2 19 187 0.1483124 3 14 102 -0.6240162 4 5 -1 0.0290889"
   "77 -0.5070168 6 13 238 -0.7516458 7 8 -1 0.0691733 43 0.04761904 9 10 -1 -0.0339981 34 0 11 12 -1 -0.0744201 -1 0.0225744 -1 -0.0135168 280 0.7956989 15 16 -1 0.0510016 172 -0.8227846 17 18 -1 0.0710675 -1 -0.036318 38 0.2363636 20 25 24 0.9011805 21 24 132 -0.8749998 22 23 -1 0.0775349 -1 -0.0170031 -1 0.13064 -1 -0.0563702 24 0.3184875 27 28 -1 0.0302133 -1 -0.0696383 29 26 0.8304324 1 10 282 159 2 9 281 134 3 8 164 0.02753455 4 7 151 -0.6923075 5 6 -1 0.0409548 -1 0.00131992 -1 0.035978 -1 -0.0140076 -1 -0.0366227 40 1.666666 11 26 33 0.9488894 12 25 265 -2.035714 13 16 37 1.148148 14 15 -1 0.0117527 -1 0.591976 255 -0.5997134 17 24 129 0.7999994 18 21 4 0.8357035 19 20 -1 -0.0121966 -1 0.267442 280 0.5526316 22 23 -1 0.608393 -1 -0.038673 -1 -0.0233532 -1 -0.0623673 109 0.4999997 27 28 -1 0.469753 -1 0.00637893 29 25 0.2011267 1 14 237 -0.4393043 2 5 277 4.5 3 4 -1 -0.0353911 -1 0.0275268"
   "280 0.8148148 6 7 -1 -0.0737483 24 0.2770544 8 13 200 1.447916 9 10 -1 -0.0707449 225 -0.7999994 11 12 -1 0.428144 -1 -0.0679908 -1 0.0638309 40 0.09090907 15 16 -1 -0.0359467 13 -0.8333328 17 20 220 -1.483333 18 19 -1 0.069915 -1 -0.0477319 186 0.09374812 21 28 26 0.8520972 22 27 254 0.0235052 23 26 70 -0.3892905 24 25 -1 0.0256955 -1 -0.0361621 -1 0.0299093 -1 0.0442677 -1 0.00192746 29 142 -0.02265946 1 16 65 -0.6666664 2 7 38 0.03333333 3 4 -1 -0.063328 164 -0.005556129 5 6 -1 0.0165497 -1 -0.0477747 280 0.04487179 8 11 281 104 9 10 -1 -0.0170539 -1 0.0813887 281 11 12 13 -1 0.0471663 145 -0.07825006 14 15 -1 -0.00780334 -1 0.0172399 33 0.8619754 17 18 -1 -0.0453286 179 0.07377048 19 28 117 0.1256258 20 27 226 -5.73214 21 22 -1 0.392636 170 0.1938396 23 24 -1 -0.0234545 218 -0.7079207 25 26 -1 0.0991888 -1 0.00313215 -1 0.0429038 -1 -0.0264372 29 30 0.597575 1 28 36 0.6458333 2 23 32"
   "0.03929301 3 4 -1 0.0680372 42 0 5 8 65 -0.9090906 6 7 -1 0.0474574 -1 -0.0778529 139 0.468045 9 22 48 -1.475249 10 11 -1 0.0477612 200 62500 12 21 255 0.185532 13 20 141 -0.8927635 14 15 -1 -0.0066724 225 -0.6999999 16 17 -1 -0.0357852 18 -0.2222221 18 19 -1 0.0497766 -1 0.0143626 -1 -0.0130352 -1 -0.0823331 -1 0.0682639 42 1.166667 24 27 38 0.06521739 25 26 -1 0.00638473 -1 -0.0260046 -1 0.0511704 -1 -0.0647976 29 30 0.8175334 1 20 203 -1.210526 2 3 -1 0.0444142 176 -0.7538461 4 7 105 1.552631 5 6 -1 -0.0196404 -1 0.106721 31 1.017587 8 19 146 0.2182776 9 18 120 0.161927 10 15 48 -0.2817434 11 12 -1 0.0174685 253 -0.3061738 13 14 -1 0.0520536 -1 -0.0225592 110 0.4821427 16 17 -1 0.0453441 -1 -0.017094 -1 -0.012724 -1 0.0534904 33 0.964784 21 28 99 1.390736 22 27 3 1.044762 23 24 -1 -0.0152996 78 0.7939313 25 26 -1 -0.0413782 -1 0.585962 -1 0.212814 -1 -0.052311 29 26 0.1791803 1 2 -1 -0.0616464"
   "26 0.9475663 3 20 282 15 4 5 -1 -0.0493072 13 -0.576271 6 7 -1 -0.0255022 232 -0.03630992 8 11 230 0.1838787 9 10 -1 0.0267006 -1 -0.0274215 30 0.3055654 12 15 280 0.8579545 13 14 -1 -0.0671687 -1 0.0900109 23 0.9086593 16 19 187 -0.06672079 17 18 -1 0.0181362 -1 -0.0213699 -1 0.0507518 232 -0.4785306 21 22 -1 -0.076054 5 -0.6672919 23 28 138 -0.09112881 24 25 -1 0.0180791 31 0.2803061 26 27 -1 -0.149147 -1 -0.0137269 -1 0.0387833 29 204 0 1 16 162 -0.1119943 2 9 19 -2.206896 3 4 -1 -0.0674863 228 0.2083333 5 6 -1 0.035758 175 -1.749993 7 8 -1 0.223625 -1 -0.00388625 111 -0.5833331 10 11 -1 -0.0500141 64 0.1666664 12 15 48 0.1040259 13 14 -1 -0.00397943 -1 0.0430898 -1 -0.0246728 185 0.07175724 17 20 129 -0.1428571 18 19 -1 -0.0104042 -1 -0.0453292 217 -347.2222 21 22 -1 0.196234 71 0.273887 23 26 47 0.1023336 24 25 -1 0.0051995 -1 -0.0319173 74 -0.3218126 27 28 -1 0.0737198 -1 -0.00281174"
   "29 30 0.597575 1 20 59 -0.2105263 2 9 33 0.9210136 3 6 83 -0.3529411 4 5 -1 0.0487914 -1 0.0162866 240 0.0001552723 7 8 -1 -0.0172022 -1 0.0196742 235 -0.6680798 10 13 270 0.5740739 11 12 -1 0.0561039 -1 -0.0463466 280 0.7247191 14 19 281 56 15 18 186 0.1049297 16 17 -1 0.000957313 -1 0.0640505 -1 -0.00904335 -1 -0.039707 26 0.7134844 21 28 5 -1.668421 22 23 -1 0.199349 138 0.1379809 24 25 -1 -0.054496 153 0.2812499 26 27 -1 -0.0235047 -1 0.21289 -1 -0.0739086 29 30 0.8569743 1 22 282 155 2 21 195 -1.160714 3 6 110 -0.6666662 4 5 -1 0.00243413 -1 0.0611119 176 -0.7656249 7 12 127 -0.4999998 8 9 -1 -0.04976 105 1.552631 10 11 -1 -0.00664172 -1 0.124965 12 1.499997 13 20 48 -0.2674477 14 17 281 26 15 16 -1 0.0736136 -1 0.0159709 224 0.8749997 18 19 -1 -0.010751 -1 0.0494351 -1 0.0588234 -1 -0.0290716 162 0.2752873 23 24 -1 -0.0489432 154 -0.1999998 25 28 176 0.1499999 26 27 -1 0.00626346 -1"
   "0.242686 -1 -0.0226925 29 26 0.1791803 1 2 -1 -0.0593248 26 0.9062614 3 12 231 -0.0270429 4 7 277 4.5 5 6 -1 -0.00934071 -1 0.026372 25 0.3273253 8 9 -1 -0.0611549 48 -0.5806253 10 11 -1 0.0346814 -1 -0.0240934 82 -0.8347825 13 16 139 -0.05349758 14 15 -1 -0.00432891 -1 -0.104922 232 -0.3057323 17 20 256 -0.2698781 18 19 -1 -0.114096 -1 0.0103643 133 0.05999999 21 28 204 2.749996 22 27 2 1.123138 23 26 206 -1.468749 24 25 -1 -0.137534 -1 0.0220844 -1 -0.124592 -1 -0.444153 -1 0.0404043 29 193 -0.03439538 1 16 111 -0.5833331 2 9 38 0.03773585 3 4 -1 -0.0587417 164 0.008853647 5 8 281 114 6 7 -1 -0.0161836 -1 0.05616 -1 -0.0576665 280 0.04487179 10 11 -1 0.06474 281 18 12 15 203 0.3928569 13 14 -1 0.0552779 -1 -0.00364693 -1 0.00734442 185 0.03024073 17 18 -1 -0.0380343 117 0.1256258 19 24 184 0.8103534 20 21 -1 -0.0134616 24 0.1575761 22 23 -1 -0.0595647 -1 0.0898313 204 2.749996 25 28 28"
   "0.8237831 26 27 -1 0.0345252 -1 -0.0399757 -1 0.429808 29 30 0.597575 1 24 31 0.1108186 2 3 -1 0.0419001 42 0 4 7 65 -0.9090906 5 6 -1 0.067534 -1 -0.0793655 280 0.7288136 8 21 141 0.424184 9 20 38 0.2264151 10 13 125 0.6716973 11 12 -1 0.0124669 -1 0.0701709 26 0.35824 14 15 -1 -0.0404381 134 0.8749991 16 19 97 -0.05458774 17 18 -1 0.0390158 -1 -0.0367421 -1 0.107174 -1 0.0640932 94 -0.880709 22 23 -1 0.0204825 -1 -0.0385518 26 0.7134844 25 28 34 0.7534246 26 27 -1 0.0266737 -1 -0.0670444 -1 -0.0731215 29 26 0.2892544 1 12 197 -0.827586 2 3 -1 0.0480971 132 0.1562499 4 11 218 -1.067567 5 6 -1 -0.0489086 133 -0.9999987 7 8 -1 -0.0573034 121 0.8534997 9 10 -1 0.00565305 -1 0.0453324 -1 0.0411833 33 0.9594946 13 24 247 1.999998 14 23 12 0.8333331 15 20 255 -1.191135 16 19 116 -0.8832604 17 18 -1 -0.0342496 -1 0.245407 -1 -0.00905457 37 1.184834 21 22 -1 0.0244153 -1 0.116223 -1 0.139007 107"
   "1.499999 25 26 -1 -0.0413935 132 0.8571426 27 28 -1 0.00379215 -1 0.381264 29 25 0.1096933 1 6 237 -0.4832313 2 3 -1 -0.0125481 282 233 4 5 -1 -0.0690583 -1 0.101617 40 0.09090907 7 8 -1 -0.033233 13 -0.7472527 9 14 220 -1.483333 10 13 203 0.5480768 11 12 -1 0.0130488 -1 0.201093 -1 -0.0341353 191 0.06564295 15 26 26 0.8520972 16 23 255 0.1338065 17 22 171 0.3863448 18 19 -1 -0.0179907 181 -0.4999995 20 21 -1 0.125147 -1 0.00863404 -1 0.0342239 227 0.8854164 24 25 -1 0.0423149 -1 -0.0270829 15 0.04651162 27 28 -1 -0.02136 -1 0.0100574 29 43 0.6428571 1 22 25 0.09317186 2 9 34 0.804878 3 6 212 0.07922569 4 5 -1 -0.0129076 -1 -0.0569485 161 -0.6144665 7 8 -1 0.0732485 -1 -0.01436 41 0.04545454 10 13 248 -0.02678571 11 12 -1 0.0121052 -1 0.0605398 191 -0.00501839 14 21 226 -0.4540229 15 20 216 -0.7051091 16 17 -1 -0.0317349 227 -1.061224 18 19 -1 0.415362 -1 0.0417492 -1 -0.0181623 -1 0.0173199"
   "115 0.3731259 23 28 34 0 24 27 46 -0.2454361 25 26 -1 0.13093 -1 -0.0170888 -1 -0.0130352 -1 -0.0528227 29 30 0.597575 1 24 28 0.8564267 2 21 187 0.1483124 3 20 281 111 4 11 282 8 5 6 -1 0.0692147 71 -1.412084 7 8 -1 0.0461933 200 1.312497 9 10 -1 0.0065982 -1 -0.0526854 282 127 12 19 88 -0.4999998 13 14 -1 0.00908215 157 -0.5131578 15 16 -1 0.0171865 3 -0.2628324 17 18 -1 -0.0635795 -1 0.0308925 -1 0.0959027 -1 0.0335081 38 0.2363636 22 23 -1 -0.0045767 -1 -0.053449 26 0.7134844 25 28 5 -1.668421 26 27 -1 0.171781 -1 -0.0283276 -1 -0.0722998 29 26 0.8304324 1 18 98 0.6619189 2 13 282 144 3 8 127 0.4499996 4 7 281 146 5 6 -1 0.00585863 -1 -0.024689 -1 0.0286065 201 -0.5178568 9 12 220 -0.3114753 10 11 -1 -0.0783452 -1 0.103475 -1 -0.0529765 201 0.1249998 14 17 128 0.5769229 15 16 -1 0.0354061 -1 0.10595 -1 0.0074945 40 1.666666 19 28 33 0.9488894 20 27 237 -0.1332096 21 26 265 -1.631578 22"
   "25 24 0.9697709 23 24 -1 -0.00571004 -1 0.453411 -1 0.0270663 -1 -0.0228254 -1 -0.0572212 -1 0.212464 29 26 0.1791803 1 2 -1 -0.055227 36 0.6410256 3 20 226 93750 4 19 281 182 5 14 100 -0.6250233 6 9 211 -1.132867 7 8 -1 0.0276098 -1 -0.062299 53 -0.04098616 10 11 -1 -0.0305446 60 -0.5714284 12 13 -1 -0.0383319 -1 0.0111995 282 7 15 16 -1 -0.0598245 197 -0.3 17 18 -1 0.0651595 -1 -0.0234461 -1 0.115209 282 31 21 24 132 0.1785713 22 23 -1 -0.04244 -1 0.0166571 40 0.3499999 25 28 91 -0.4999999 26 27 -1 -0.0657896 -1 0.0117172 -1 0.0303983 29 204 -0.08333322 1 18 195 -0.05681816 2 17 145 -0.9830922 3 4 -1 -0.0573875 133 -0.8571419 5 6 -1 0.0700889 198 2.775558e-17 7 16 148 0.4837925 8 15 194 0.01902166 9 14 199 0.4166665 10 13 212 0.1340574 11 12 -1 0.0358268 -1 -0.0337788 -1 -0.0996336 -1 -0.0426054 -1 0.0228299 -1 0.0416609 -1 0.0289443 185 0.07618243 19 22 280 0.172043 20 21 -1 0.0178985"
   "-1 -0.0290268 217 -347.2222 23 26 188 0.2247062 24 25 -1 0.293637 -1 0.00891661 33 0.8569184 27 28 -1 -0.0327668 -1 0.00973277 29 30 0.597575 1 22 37 0.4615384 2 13 33 0.9765182 3 10 260 0.454362 4 9 36 0.2222222 5 6 -1 0.0484637 229 -0.5652173 7 8 -1 -0.0501134 -1 0.0262001 -1 -0.00148745 263 -0.0698543 11 12 -1 0.0118887 -1 -0.0288489 34 0 14 15 -1 -0.0686196 134 0.7249999 16 21 26 0.0006807024 17 18 -1 0.0792857 34 0.5919002 19 20 -1 0.00385522 -1 -0.0247683 -1 0.027605 26 0.7134844 23 28 34 0.7534246 24 27 138 0.2128411 25 26 -1 -0.0025632 -1 0.143366 -1 -0.0649278 -1 -0.0714742 29 26 0.2783841 1 22 151 -0.6923075 2 11 108 -0.04166664 3 6 221 0.1093749 4 5 -1 -0.016263 -1 0.0437794 122 -1.006205 7 8 -1 -0.0201843 157 -1.689655 9 10 -1 0.171387 -1 0.0650318 224 0.1249998 12 21 197 -0.07894735 13 16 50 -0.1800374 14 15 -1 -0.0186871 -1 0.0535456 189 -1.452299 17 18 -1 0.12317 258 0.4725041"
   "19 20 -1 -0.0415372 -1 0.0275214 -1 0.0198404 33 0.9653362 23 28 31 1.017587 24 27 270 -1.699999 25 26 -1 0.0925702 -1 -0.0056989 -1 0.0404533 -1 -0.0365503 29 25 0.1096933 1 10 237 -0.4614879 2 3 -1 -0.0094119 280 0.8188976 4 5 -1 -0.0724641 154 0.3124997 6 7 -1 -0.0531954 222 -1.031249 8 9 -1 0.2968 -1 0.0362913 40 0.09090907 11 14 106 -0.2499999 12 13 -1 -0.0543972 -1 -0.0106495 13 -0.8333328 15 20 220 -1.483333 16 17 -1 0.0553491 185 -0.001742938 18 19 -1 -0.0144367 -1 -0.0758662 64 0.1111111 21 26 256 -0.02329055 22 25 208 -0.4284797 23 24 -1 0.0152382 -1 -0.0288774 -1 0.0131844 53 -0.4396404 27 28 -1 -0.0126138 -1 0.0289378 29 43 0.08948545 1 18 32 0.03929301 2 3 -1 -0.0638181 40 0.04347825 4 7 273 0.1428571 5 6 -1 0.0772795 -1 0.0144648 99 0.1628114 8 9 -1 -0.0139176 35 0.3333332 10 15 281 200 11 14 51 -0.5001577 12 13 -1 0.0640777 -1 -0.0109512 -1 -0.0560647 141 0.1537582 16 17 -1"
   "0.0515264 -1 0.179819 23 0.9464414 19 28 38 0.07444168 20 23 42 0 21 22 -1 0.0739329 -1 -0.0343479 39 0.1111111 24 27 269 0.07812498 25 26 -1 0.0405674 -1 0.00381787 -1 -0.003677 -1 -0.0378861 29 25 0.5912395 1 26 59 -0.2105263 2 15 139 0.3831055 3 14 34 0.3142857 4 7 57 -0.7804875 5 6 -1 0.03873 -1 0.00227877 3 -0.6935644 8 13 198 0.2132352 9 12 48 -1.475249 10 11 -1 0.0300949 -1 -0.0363185 -1 0.0208984 -1 0.0230809 -1 0.0609779 19 1.499999 16 25 26 0.00025822 17 18 -1 0.0682019 280 0.6857143 19 22 281 57 20 21 -1 0.0308571 -1 -0.0122147 31 0.2045844 23 24 -1 0.0419846 -1 -0.0446227 -1 0.0333154 231 -0.7653779 27 28 -1 0.154544 -1 -0.0592904 29 282 138 1 26 281 146 2 23 110 0.2499995 3 22 129 0.06818181 4 15 145 -0.7117565 5 12 2 -1.079755 6 9 211 -0.1871209 7 8 -1 0.09733 -1 -0.0251376 138 -0.7073941 10 11 -1 0.0606579 -1 0.0026531 190 -0.006944398 13 14 -1 -0.0229035 -1 0.0141063 149 -0.8437496"
   "16 19 110 -0.8571424 17 18 -1 -0.0375843 -1 0.0749814 162 -0.1357166 20 21 -1 -0.00295947 -1 0.0348595 -1 -0.0080307 124 1.054939 24 25 -1 -0.0242402 -1 0.145384 201 -0.5416659 27 28 -1 0.0408541 -1 -0.0396477 29 26 0.1791803 1 2 -1 -0.0511688 36 0.6410256 3 14 226 93750 4 13 281 182 5 12 100 -0.6250233 6 7 -1 -0.0469259 14 -0.5714282 8 9 -1 -0.0398918 184 0.301957 10 11 -1 0.00884093 -1 -0.0306851 -1 0.0261445 -1 0.111374 282 31 15 18 119 0.522842 16 17 -1 -0.0107769 -1 -0.135748 26 0.8748592 19 24 255 0.06550841 20 23 115 -0.467601 21 22 -1 0.0196707 -1 -0.0286639 -1 0.0346815 281 226 25 26 -1 0.036437 189 -0.3465179 27 28 -1 0.0852548 -1 -0.381368 29 111 -1.458332 1 2 -1 -0.0448438 280 0.04487179 3 6 281 106 4 5 -1 -0.0314163 -1 0.0595688 115 0.3610797 7 28 103 -2.181818 8 9 -1 0.13109 195 -0.09999994 10 15 282 116 11 12 -1 -0.0150016 205 1.156249 13 14 -1 0.0154197 -1 0.209181 204 -0.1428571"
   "16 17 -1 0.0257108 194 -3.542437 18 19 -1 0.133043 185 -0.01803352 20 21 -1 -0.0241115 69 -0.6225456 22 23 -1 -0.0174443 13 -0.8333328 24 27 26 0.894296 25 26 -1 0.0245752 -1 0.192528 -1 0.0122731 -1 -0.0303703 29 30 0.597575 1 26 6 -1.93219 2 3 -1 0.0468946 280 0.7757848 4 21 281 97 5 12 54 -0.1734677 6 11 225 -0.642857 7 8 -1 -0.0568079 151 -0.9024389 9 10 -1 0.00114669 -1 0.0428283 -1 -0.000204351 111 -0.2142857 13 18 189 -0.6926892 14 15 -1 -0.022153 1 -0.5848952 16 17 -1 0.0116954 -1 0.0734706 282 127 19 20 -1 -0.0325543 -1 0.0621553 172 -0.9189185 22 23 -1 0.0230514 189 0.4261242 24 25 -1 -0.0657881 -1 -0.00390954 26 0.7134844 27 28 -1 -0.0111809 -1 -0.0698249 29 235 0.2328139 1 24 25 0.2011267 2 15 218 -0.6549999 3 6 197 -0.1458332 4 5 -1 0.0119175 -1 -0.0499618 186 -0.5656221 7 8 -1 0.0277594 203 16.37493 9 14 241 -1.216666 10 11 -1 0.0608148 282 126 12 13 -1 0.00638799 -1 -0.0392605"
   "-1 -0.131842 258 0.2465446 16 21 50 -0.05470979 17 18 -1 -0.0574582 192 -0.6026755 19 20 -1 0.0627585 -1 -0.0211415 15 1.125 22 23 -1 0.00732609 -1 0.18328 260 -0.5014734 25 26 -1 0.115874 40 1.666666 27 28 -1 0.0163863 -1 0.180099 29 25 0.09841012 1 10 280 0.8112245 2 5 237 -0.4614879 3 4 -1 -0.0142054 -1 -0.070877 40 0.03846153 6 7 -1 -0.0608224 226 -0.8437496 8 9 -1 0.170293 -1 0.0138239 282 15 11 12 -1 -0.0303449 187 0.02680884 13 22 41 0.05555554 14 21 248 -0.03999999 15 16 -1 0.0152202 282 201 17 18 -1 -0.0669213 201 -0.01562499 19 20 -1 -0.0339317 -1 0.192988 -1 0.0231788 100 0.1914133 23 26 32 0.6649336 24 25 -1 -0.00552147 -1 -0.0447968 238 -0.4718209 27 28 -1 -0.031038 -1 0.0229029 29 193 -0.01751975 1 14 195 -0.1785713 2 9 99 -0.95505 3 4 -1 -0.0492005 25 0.03949014 5 6 -1 -0.0418609 23 0.5127566 7 8 -1 0.0338681 -1 -0.00658317 176 0.4615382 10 11 -1 0.0227529 112 -0.09999995 12"
   "13 -1 0.00834917 -1 -0.064482 184 0.007396724 15 16 -1 -0.0395216 26 1.008762 17 26 26 0.03169194 18 19 -1 -0.05659 184 0.8103534 20 25 110 0.08999999 21 24 36 0.9272727 22 23 -1 0.00190182 -1 0.0487504 -1 -0.0165525 -1 0.0489727 28 0.4920497 27 28 -1 0.207436 -1 0.00492272 29 30 0.597575 1 26 36 0.55 2 15 185 0.3873175 3 14 128 -0.01785714 4 9 33 0.9273937 5 8 266 -0.5416663 6 7 -1 -0.0767817 -1 0.030976 -1 0.000240694 197 -0.02343749 10 13 100 -0.448854 11 12 -1 0.0576234 -1 -0.0134847 -1 -0.0335438 -1 0.0428038 46 -1.400637 16 17 -1 0.0373001 134 0.5416665 18 23 24 0 19 20 -1 0.111369 34 0.745098 21 22 -1 -0.00973011 -1 -0.0414982 33 0.9787734 24 25 -1 0.0367158 -1 -0.028249 26 0.7134844 27 28 -1 -0.00987215 -1 -0.0688748 29 266 0.4533332 1 28 282 143 2 25 127 0.7499995 3 20 26 0.2461301 4 11 203 -1.521739 5 10 34 0.1666666 6 7 -1 -0.0129394 125 0.5927052 8 9 -1 0.115675 -1 -0.0488024"
   "-1 0.00500118 44 1.263158 12 19 254 -0.1033859 13 16 23 1.1054 14 15 -1 0.0011235 -1 0.205261 255 0.3449015 17 18 -1 -0.0356248 -1 0.000937935 -1 0.0810204 208 0.06567104 21 24 195 -1.335227 22 23 -1 0.107824 -1 -0.0191146 -1 0.0511892 201 -0.5624989 26 27 -1 0.0365328 -1 -0.0467947 -1 0.0231175 29 25 0.09841012 1 14 280 0.8112245 2 7 237 -0.4153438 3 4 -1 -0.0143959 160 1.799997 5 6 -1 -0.0719032 -1 0.249614 40 0.03846153 8 9 -1 -0.0590799 199 0.06249999 10 13 226 -1.231132 11 12 -1 0.315732 -1 0.0719493 -1 -0.0265686 40 0.09090907 15 16 -1 -0.0250286 239 0.1832153 17 22 238 -0.4718209 18 19 -1 -0.0156172 282 7 20 21 -1 -0.0424117 -1 0.0189563 146 0.0776321 23 28 254 -0.0961801 24 25 -1 -0.0601744 231 0.7483835 26 27 -1 -0.0187296 -1 0.0572487 -1 0.0108846 29 254 0.03998705 1 26 24 0.9479422 2 21 88 -0.6666665 3 6 38 0.03333333 4 5 -1 -0.047427 -1 0.00317286 260 -0.02541074 7 10 233 -0.08693521"
   "8 9 -1 -0.0223638 -1 0.0124481 34 0.05555554 11 16 264 0.02272726 12 15 29 0.2240475 13 14 -1 -0.0685694 -1 0.131751 -1 0.0238366 122 -0.9585515 17 20 7 -0.985221 18 19 -1 0.02482 -1 0.165523 -1 0.0174309 54 0 22 25 256 -0.1882755 23 24 -1 0.120061 -1 -0.0119305 -1 -0.0443504 99 0.1796637 27 28 -1 -0.0188693 -1 0.0114175 29 30 0.597575 1 24 235 -0.7361713 2 5 115 0.02409231 3 4 -1 0.049183 -1 -0.0605604 105 -0.01694915 6 17 151 -0.9999998 7 12 131 -0.2499998 8 9 -1 0.0170997 224 -0.8979589 10 11 -1 -0.17196 -1 -0.0224155 230 0.04873023 13 16 121 0.3033689 14 15 -1 -0.00367568 -1 0.027114 -1 0.0320184 47 -1.990766 18 19 -1 0.0501495 19 0.9090906 20 21 -1 -0.0210302 127 -2.124996 22 23 -1 0.251658 -1 0.00776379 26 0.6657491 25 28 34 0.7297297 26 27 -1 0.0462368 -1 -0.0617042 -1 -0.0659359 29 238 0.08563418 1 16 26 0.5611015 2 9 214 -0.9735563 3 4 -1 -0.0433555 199 -0.2152777 5 6 -1 0.0173736"
   "282 119 7 8 -1 0.0018115 -1 -0.0551074 27 0.8148781 10 11 -1 -0.0455412 258 0.2732738 12 15 3 1.044762 13 14 -1 -0.0161765 -1 0.276071 -1 0.08301 260 -0.1261798 17 18 -1 0.0496893 7 -0.2296039 19 26 261 -0.05598786 20 25 122 -0.6363729 21 22 -1 -0.0177587 5 0.3293123 23 24 -1 -0.0127291 -1 0.0501751 -1 0.0327014 208 0.7436742 27 28 -1 -0.0202628 -1 0.0472438 29 26 0.1791803 1 4 231 -0.582323 2 3 -1 -0.00642354 -1 -0.0597777 26 0.9557858 5 18 282 15 6 7 -1 -0.0379693 207 0.3587271 8 17 187 0.1483124 9 16 204 0.3984374 10 15 176 -0.3248731 11 14 118 -0.7783378 12 13 -1 0.013355 -1 -0.0380072 -1 0.0158137 -1 0.0380456 -1 -0.0156771 -1 -0.0246491 26 1.017338 19 28 133 0.1833333 20 27 206 -1.359374 21 22 -1 -0.119835 204 2.749996 23 26 61 0.9215686 24 25 -1 0.0184857 -1 -0.0933677 -1 -0.322548 -1 0.0369972 -1 -0.0548189 29 43 0.6428571 1 20 48 -2.505886 2 3 -1 -0.0513662 263 -0.01630428 4 11 119"
   "-0.2356028 5 10 282 110 6 9 188 -0.5581903 7 8 -1 -0.00780872 -1 -0.0488783 -1 0.0192221 -1 0.0108794 33 0.9158995 12 15 280 0.1052632 13 14 -1 0.0543731 -1 -0.0202153 282 7 16 17 -1 0.0603537 184 -0.01489236 18 19 -1 0.00688586 -1 0.0390159 163 -0.09696542 21 28 184 0.7837237 22 25 204 -0.07499994 23 24 -1 0.0256339 -1 -0.0157738 39 0.2564101 26 27 -1 -0.0364368 -1 0.127951 -1 -0.0251891 29 25 0.5912395 1 26 200 93750 2 25 37 0.4705881 3 22 187 -0.8875519 4 9 281 96 5 6 -1 0.0147499 198 -0.4886362 7 8 -1 -0.0577156 -1 0.00996689 281 27 10 13 29 0.2457172 11 12 -1 0.0574138 -1 -0.0316709 187 0.4449096 14 21 38 0.01923077 15 18 24 0.1090824 16 17 -1 0.0546807 -1 -0.0017641 56 0.1239277 19 20 -1 0.0268138 -1 -0.01026 -1 0.0703908 63 -0.5999998 23 24 -1 0.0167314 -1 -0.0120483 -1 -0.0710089 23 0.3018157 27 28 -1 0.0929174 -1 -0.0546244 29 267 0.7142853 1 28 244 0.3863636 2 21 30 0.8569743 3"
   "16 157 -2.857142 4 5 -1 0.0621205 96 1.433113 6 15 247 0.9166661 7 14 282 92 8 11 268 -0.1249999 9 10 -1 0.0271579 -1 -0.00599205 187 -0.2256836 12 13 -1 -0.00475432 -1 -0.0365932 -1 0.0427142 -1 0.0843981 98 1.078116 17 18 -1 -0.0440161 180 0.2142857 19 20 -1 -0.0296531 -1 0.286348 23 1.054564 22 25 273 -0.7083332 23 24 -1 0.0427866 -1 0.00604107 265 -1.812499 26 27 -1 0.365428 -1 0.0891165 -1 0.0267707 29 25 0.07137809 1 10 280 0.8112245 2 7 114 1.548386 3 4 -1 -0.0601228 188 0.4026059 5 6 -1 -0.0519508 -1 0.36105 24 0.1324264 8 9 -1 -0.0391627 -1 0.0758152 13 -0.8333328 11 14 220 -1.483333 12 13 -1 0.0358141 -1 -0.0365802 63 -0.07692305 15 18 41 0.08333331 16 17 -1 -0.029922 -1 0.000230794 226 3.374998 19 28 230 0.0669812 20 25 140 1.177837 21 24 138 0.7069278 22 23 -1 0.0244102 -1 -0.0316097 -1 -0.100144 254 -0.1629534 26 27 -1 -0.0389218 -1 0.00970118 -1 0.109316 29 111 -1.458332 1 2"
   "-1 -0.039847 39 0 3 22 38 0.02127659 4 17 146 -0.11902 5 16 157 -0.4999998 6 7 -1 -0.0554189 31 0.1393998 8 9 -1 -0.0515551 33 0.753124 10 13 149 0.3055554 11 12 -1 0.203865 -1 -0.02458 170 1.028514 14 15 -1 0.0082035 -1 0.34901 -1 -0.0656495 49 -0.06356723 18 19 -1 0.0348079 282 241 20 21 -1 0.00160046 -1 0.161356 171 -1.60289 23 28 153 -1.281553 24 27 174 -0.4062498 25 26 -1 0.355925 -1 0.0237719 -1 0.0200032 -1 -0.00643484 29 30 0.597575 1 22 102 -0.8866077 2 3 -1 0.0180109 141 0.4920488 4 21 28 0.8564267 5 20 200 62500 6 19 281 113 7 14 280 0.7757848 8 13 73 -0.3657849 9 10 -1 0.035296 23 0.5184914 11 12 -1 -0.0076435 -1 0.0356788 -1 -0.0256723 38 0.03333333 15 16 -1 0.0208633 217 -1.669198 17 18 -1 0.0718793 -1 -0.0396101 -1 -0.0876306 -1 -0.0294888 -1 0.041176 26 0.7134844 23 24 -1 -0.0049963 222 -4.861109 25 28 138 0.3240207 26 27 -1 -0.0485058 -1 0.490854 -1 -0.0690493 29 98 0.876807"
   "1 24 38 0.8823529 2 19 247 0.3055554 3 16 266 0.499998 4 15 282 141 5 14 151 -0.6923075 6 13 281 65 7 12 224 -0.7857138 8 9 -1 0.333276 221 0.08152172 10 11 -1 -0.0138363 -1 0.0950721 -1 0.00443099 -1 -0.00710935 -1 -0.0384742 -1 0.016491 107 0.8586955 17 18 -1 0.0211649 -1 0.192577 15 0.3728813 20 21 -1 -0.0538439 273 0.940476 22 23 -1 -0.0118497 -1 0.151603 223 0.1999998 25 26 -1 0.0521135 281 33 27 28 -1 0.0861964 -1 0.000995988 29 25 0.09841012 1 6 233 -0.2639627 2 3 -1 -0.00674743 282 233 4 5 -1 -0.0624026 -1 0.0888284 53 1.117635 7 28 234 0.2403442 8 25 14 -0.3333333 9 20 184 -0.0101836 10 19 26 0.9337446 11 16 282 206 12 13 -1 -0.0148934 60 -0.7999994 14 15 -1 0.199998 -1 0.0192946 157 0.4081631 17 18 -1 0.0581754 -1 -0.023489 -1 -0.0260464 98 -1.940465 21 22 -1 0.262914 164 0.9765914 23 24 -1 0.0158556 -1 -0.147841 282 70 26 27 -1 -0.0312642 -1 0.00699253 -1 -0.0503578 29 240 0.009969269"
   "1 22 112 0.2999999 2 17 195 0.1216216 3 4 -1 0.00436297 281 146 5 10 219 -2.142856 6 9 137 -0.5326086 7 8 -1 -0.297387 -1 0.0526015 -1 0.0158043 197 1.567307 11 16 151 -2.147058 12 15 279 15.5 13 14 -1 0.0503093 -1 -0.236398 -1 0.0679173 -1 -0.149034 216 0.7320154 18 19 -1 -0.0217983 214 -3.347321 20 21 -1 0.321932 -1 0.0291019 185 0.02247347 23 28 196 0.008333331 24 27 102 0.4011395 25 26 -1 -0.0183637 -1 0.0220033 -1 -0.0368062 -1 0.00465273 29 30 0.597575 1 22 76 -1.834151 2 3 -1 0.0272854 200 62500 4 21 3 -0.7066767 5 14 51 -1.624161 6 7 -1 0.0462348 198 0.1071428 8 13 62 -0.1249999 9 12 30 0.08637716 10 11 -1 0.00304489 -1 -0.0540309 -1 -0.037626 -1 0.00630863 107 -0.7222221 15 16 -1 0.0572129 18 -0.1764705 17 18 -1 0.0263341 30 0.003966896 19 20 -1 -0.0618289 -1 -0.00360389 -1 -0.0761502 26 0.7937791 23 28 38 0.6271186 24 27 84 0.3673469 25 26 -1 0.0370481 -1 0.5036 -1 -0.0263962 -1"
   "-0.0681147 29 28 1.048378 1 24 258 0.1653302 2 21 282 167 3 20 152 -0.1111111 4 11 50 -0.4696324 5 6 -1 -0.0203186 23 0.1427295 7 8 -1 0.109466 213 -2.281901 9 10 -1 0.213839 -1 0.0150157 35 1.405405 12 19 281 127 13 18 200 -0.7999994 14 17 25 0.8859729 15 16 -1 0.049705 -1 0.424147 -1 -0.0132192 -1 -0.0476043 -1 0.0619487 -1 -0.0554512 15 1.125 22 23 -1 0.0128689 -1 0.148236 183 -2.775558e-17 25 28 255 -0.8513056 26 27 -1 0.356538 -1 0.0723356 -1 0.0171501 29 24 0.1203412 1 2 -1 -0.0503518 36 0.5263157 3 14 281 182 4 13 210 0.2581381 5 12 195 0.6363634 6 11 61 0.02083333 7 8 -1 -0.0340079 60 -0.4999999 9 10 -1 -0.0341989 -1 0.0106603 -1 0.0401745 -1 -0.054117 -1 0.0317921 282 31 15 20 4 0.2298767 16 19 63 0.3333329 17 18 -1 -0.0236223 -1 0.0345372 -1 -0.0790275 40 0.3499999 21 24 22 -0.3870966 22 23 -1 -0.0441302 -1 0.00651091 187 0.02895286 25 26 -1 0.033659 217 -347.2222 27 28 -1 -0.116586"
   "-1 0.0112635 29 254 0.04663396 1 26 24 0.9479422 2 21 88 -2.299999 3 4 -1 -0.0385119 4 0.2582155 5 18 282 105 6 11 282 32 7 8 -1 0.0139042 219 0.1029412 9 10 -1 -0.00288215 -1 -0.0410066 282 203 12 13 -1 0.0304501 281 52 14 17 10 -0.2699347 15 16 -1 0.0877679 -1 -0.0330315 -1 0.324141 33 0.9180994 19 20 -1 -0.000564772 -1 0.0491998 232 -0.04770237 22 25 231 0.2181087 23 24 -1 -0.0194632 -1 0.127057 -1 -0.0478066 99 0.05185569 27 28 -1 -0.0195746 -1 0.00622049 29 25 0.619265 1 26 40 0.2777777 2 11 25 0.07137809 3 6 138 -0.6738805 4 5 -1 0.00460229 -1 0.0279588 26 0.1370974 7 8 -1 -0.0314306 270 0.5121949 9 10 -1 0.030664 -1 -0.0238614 26 0.35824 12 15 28 0.5764525 13 14 -1 -0.00174416 -1 -0.0381403 30 0.02698445 16 17 -1 -0.0318432 5 0.06361131 18 23 19 0.9999996 19 22 140 -0.8272364 20 21 -1 0.102719 -1 0.0193945 -1 0.124116 25 0.2340678 24 25 -1 0.016955 -1 -0.0482509 7 2.030622 27 28 -1"
   "-0.0493293 -1 0.125687 29 247 1.999998 1 28 26 0.2461301 2 13 245 0.1770833 3 12 197 -0.827586 4 5 -1 0.0335069 224 0.006944443 6 11 209 -0.1589345 7 10 209 -0.1635287 8 9 -1 -0.00467646 -1 0.192968 -1 -0.0415559 -1 0.0123275 -1 0.0763221 33 0.964784 14 23 260 -0.61343 15 18 31 1.06988 16 17 -1 0.0410711 -1 0.225137 260 0.1917501 19 22 137 1.05 20 21 -1 -0.013651 -1 0.0511693 -1 0.0165997 178 1.17647 24 25 -1 -0.0345813 282 76 26 27 -1 0.171509 -1 -0.0476324 -1 0.0840255 29 24 0.1203412 1 2 -1 -0.0483581 226 93750 3 28 36 0.5263157 4 21 27 0.4470114 5 12 24 0.5535391 6 7 -1 -0.0227085 155 0.3124997 8 9 -1 0.0486657 281 187 10 11 -1 -0.0455321 -1 0.0768674 28 0.47298 13 18 206 0.1634615 14 15 -1 -0.0294202 281 19 16 17 -1 0.0404535 -1 -0.129934 195 0.5113635 19 20 -1 -0.0200412 -1 0.0260215 110 0.2222221 22 23 -1 -0.00283943 28 0.257578 24 25 -1 -0.130832 99 -0.438223 26 27 -1 -0.0163071 -1"
   "0.0288292 -1 0.104254 29 280 0.03669725 1 4 281 104 2 3 -1 -0.0446278 -1 0.0499067 111 -1.394736 5 6 -1 -0.0377832 103 -2.181818 7 8 -1 0.112669 92 0.2386694 9 24 93 -0.0364264 10 17 204 0.3928569 11 16 179 -0.9038459 12 13 -1 0.0434006 130 0.2583732 14 15 -1 0.00553448 -1 -0.0203255 -1 -0.02447 230 0.0669812 18 23 37 0.4999995 19 20 -1 0.0239641 187 0.1125598 21 22 -1 -0.0297564 -1 0.0256316 -1 0.0489778 184 0.8459976 25 28 217 -18.34512 26 27 -1 0.155129 -1 -0.0271541 -1 0.0778962 29 30 0.597575 1 12 19 2.166665 2 9 264 -1.629629 3 4 -1 -0.0472716 59 -0.2857142 5 8 33 0.949754 6 7 -1 0.0212222 -1 -0.00239627 -1 -0.00519111 30 0.4376178 10 11 -1 0.0363097 -1 0.296709 26 0.7937791 13 24 141 0.2168695 14 17 5 -1.668421 15 16 -1 0.146728 -1 -0.0406224 7 0.06696685 18 21 226 -0.08108105 19 20 -1 0.025031 -1 0.503311 7 2.030622 22 23 -1 -0.0290834 -1 0.310526 222 -4.861109 25 28 223 -0.8749965"
   "26 27 -1 0.467885 -1 -0.0463861 -1 -0.070581 29 98 0.6619189 1 18 38 0.9189189 2 17 247 0.9166661 3 16 222 0.3862275 4 9 219 -0.5833326 5 6 -1 -0.0376086 203 16.37493 7 8 -1 -9.80173e-05 -1 -0.101576 186 -0.8021023 10 13 220 -0.2083333 11 12 -1 0.199058 -1 0.0323419 224 0.8749997 14 15 -1 0.00187766 -1 0.0658647 -1 0.0407984 -1 -0.0333903 134 0.01851851 19 24 197 -0.9295774 20 23 202 0.08333322 21 22 -1 0.0735297 -1 -0.0429812 -1 -0.00325301 38 0.9999999 25 26 -1 0.0345692 128 -0.6874997 27 28 -1 0.411926 -1 0.0716697 29 25 0.07137809 1 10 280 0.8112245 2 7 114 1.548386 3 4 -1 -0.0546931 130 -0.9565217 5 6 -1 0.318031 -1 -0.0475246 40 0.03846153 8 9 -1 -0.0500301 -1 0.0596153 53 1.117635 11 28 234 0.2403442 12 27 238 -0.5424589 13 14 -1 -0.0196835 26 0.9557858 15 26 231 -0.0270429 16 25 260 0.4301261 17 24 282 213 18 21 253 -0.2568771 19 20 -1 0.0296797 -1 -0.00711932 115 0.001757036 22 23"
   "-1 0.100904 -1 -0.0854052 -1 0.0461576 -1 -0.00876222 -1 0.0244571 -1 -0.0154153 -1 -0.0456561 29 157 -1.511904 1 2 -1 -0.0368523 280 0.03669725 3 8 281 104 4 5 -1 -0.0386025 133 0.3749993 6 7 -1 0.0685404 -1 -0.0370413 149 -2.124999 9 10 -1 0.0991933 26 1.020031 11 28 271 0.04166665 12 27 41 0.03039513 13 26 76 0.1783735 14 25 18 -0.3636361 15 18 70 -0.09573462 16 17 -1 -0.00753565 -1 -0.158507 170 0.970951 19 22 17 -0.1355932 20 21 -1 0.00216941 -1 0.0966028 76 -0.09058642 23 24 -1 0.019243 -1 0.506809 -1 0.0628645 -1 0.000282043 -1 -0.00882158 -1 0.069747 29 38 0.2833333 1 14 40 0.3225806 2 5 139 0.7092463 3 4 -1 0.00950014 -1 0.0744036 221 0.1145833 6 9 10 1.11003 7 8 -1 -0.0330644 -1 0.106731 200 3.469447e-17 10 11 -1 -0.0158321 3 0.4134245 12 13 -1 0.0235696 -1 0.244676 26 0.8586217 15 26 26 0.2988632 16 19 111 2.749995 17 18 -1 -0.0450663 -1 0.219956 281 135 20 25 5 0.2394727 21 24"
   "39 0.03875968 22 23 -1 -0.0367917 -1 0.0482963 -1 -0.0375235 -1 -0.0528434 25 0.2604601 27 28 -1 0.0405267 -1 -0.068966 29 267 0.6999994 1 28 37 1.106383 2 27 145 -0.03241167 3 20 222 0.3862275 4 19 198 -0.2016129 5 14 158 -0.4999998 6 9 3 -0.8961077 7 8 -1 0.0166152 -1 -0.0481331 254 0.06061337 10 13 220 -0.3114753 11 12 -1 -0.00585221 -1 0.0503713 -1 0.00125544 281 89 15 18 209 0.02671797 16 17 -1 -0.00861614 -1 0.0251 -1 -0.0309547 -1 0.0376318 32 0.4256415 21 22 -1 -0.0454839 96 1.101547 23 26 94 1.444754 24 25 -1 -0.0120122 -1 0.119036 -1 0.0980388 -1 0.0308474 -1 0.0219697 29 24 0.1203412 1 2 -1 -0.0448969 226 93750 3 28 234 0.09597979 4 25 40 0.08333331 5 14 223 -0.4999999 6 13 43 0.1428571 7 8 -1 -0.0426512 121 -0.9615296 9 12 205 0.0892857 10 11 -1 0.408286 -1 0.00298468 -1 0.0501596 -1 -0.0313434 121 0.583997 15 20 59 -0.7499998 16 17 -1 -0.0170159 52 -2.071412 18 19 -1 0.227846"
   "-1 0.0157993 122 0.7115219 21 24 281 103 22 23 -1 -0.0298483 -1 0.027809 -1 -0.168543 256 -0.02450237 26 27 -1 -0.0279811 -1 -0.0014474 -1 0.0982106 29 240 0.009969269 1 24 112 0.2999999 2 23 195 0.1216216 3 14 145 -2.474802 4 5 -1 -0.0763575 168 1.138015 6 13 26 0.9316518 7 10 85 -0.3333331 8 9 -1 -0.0110662 -1 0.0205389 256 -0.05594877 11 12 -1 0.013486 -1 -0.0449143 -1 0.0896553 281 146 15 16 -1 0.0118968 203 -0.4249998 17 18 -1 -0.138776 151 -2.147058 19 22 279 14.5 20 21 -1 0.0552722 -1 -0.200086 -1 0.0623413 -1 -0.012451 202 -0.1666666 25 26 -1 -0.0230185 228 0.2857142 27 28 -1 0.007806 -1 -0.0197439 29 38 0.5192307 1 22 3 -0.7066767 2 13 71 -1.412084 3 4 -1 0.0205723 38 0.1833333 5 12 202 0.07812496 6 11 147 -0.6914774 7 8 -1 0.0129817 71 -1.331358 9 10 -1 -0.145682 -1 -0.0212889 -1 0.00995255 -1 -0.0346243 107 -0.7480915 14 15 -1 0.0549028 138 -1.197249 16 17 -1 0.100906 18 -0.1249999"
   "18 19 -1 0.0195618 260 -0.01432061 20 21 -1 0.0291539 -1 -0.0323173 26 0.8586217 23 28 34 0.5263157 24 25 -1 0.0285494 19 2.571428 26 27 -1 -0.0374308 -1 0.16315 -1 -0.0660798 29 266 0.5428571 1 28 157 -1.165517 2 7 99 -1.003297 3 6 226 0.6224488 4 5 -1 -0.0136884 -1 0.148567 -1 0.0443271 210 0.1336385 8 25 282 130 9 22 96 1.433113 10 21 102 -1.895197 11 12 -1 -0.0531095 24 0.3373174 13 18 200 62500 14 17 179 -0.4499996 15 16 -1 -0.0243034 -1 0.0139542 -1 0.12744 270 -0.7222219 19 20 -1 0.0254426 -1 -0.0266643 -1 0.129174 151 1.134615 23 24 -1 -0.052811 -1 0.0606113 127 0.9166664 26 27 -1 0.00486226 -1 0.0517822 -1 0.0184526 29 25 0.149626 1 12 233 -0.2639627 2 5 262 -0.2156169 3 4 -1 0.047203 -1 -0.0158344 216 -0.6928078 6 11 200 1.656249 7 10 24 0.3280498 8 9 -1 -0.0579545 -1 0.103201 -1 0.340503 -1 -0.0608465 53 1.117635 13 28 202 -0.215909 14 19 282 209 15 18 26 0.9234137 16 17 -1 0.000220677"
   "-1 0.0382195 -1 0.106104 282 241 20 27 36 0.5445544 21 26 281 183 22 25 195 0.6363634 23 24 -1 -0.0245234 -1 0.0442954 -1 0.0466341 -1 0.00740826 -1 -0.106923 -1 -0.0413661 29 70 -1.592347 1 2 -1 -0.0295117 39 0 3 20 38 0.02127659 4 11 280 0.9090909 5 8 281 144 6 7 -1 -0.052533 -1 0.0347139 134 -0.6666664 9 10 -1 -0.0241058 -1 0.102662 209 0.6605638 12 19 63 -0.7199999 13 14 -1 0.0548303 201 -0.4999999 15 18 208 -0.03309254 16 17 -1 0.0207826 -1 -0.060408 -1 0.0224836 -1 -0.0321445 191 -0.9303334 21 22 -1 -0.0385497 81 0.4999998 23 28 7 -0.8262832 24 25 -1 0.0298286 3 -0.1402249 26 27 -1 -0.0153516 -1 0.00827005 -1 -0.0200313 29 38 0.5192307 1 12 141 0.5485227 2 11 227 0.1249999 3 8 41 0.2777777 4 5 -1 0.00245882 225 -0.1964285 6 7 -1 -0.0607021 -1 -0.0137197 36 0.7999999 9 10 -1 0.0148635 -1 -0.0121693 -1 0.0405953 26 0.8586217 13 28 281 113 14 27 282 63 15 24 211 0.09700508 16 23 3 -0.4373071"
   "17 22 213 -0.06730707 18 19 -1 0.0177945 213 0.2570612 20 21 -1 0.463017 -1 0.0819223 -1 0.0553466 -1 0.0036615 223 -1.449999 25 26 -1 0.329405 -1 -0.0418634 -1 -0.0572519 -1 -0.0649438 29 28 1.048378 1 26 258 0.03925923 2 15 282 91 3 10 55 0.1628543 4 9 25 0.0007659527 5 8 102 -2.126986 6 7 -1 -0.0679972 -1 0.0814505 -1 -0.0172824 -1 0.0149978 235 0.2588669 11 12 -1 -0.0345776 9 0.7249656 13 14 -1 0.0356449 -1 -0.0361567 53 -0.05139519 16 21 277 4.5 17 18 -1 0.0244592 101 -0.9693676 19 20 -1 0.0545543 -1 -0.0031492 53 2.042717 22 25 33 0.9580869 23 24 -1 0.0023902 -1 -0.0359757 -1 0.115036 183 -2.775558e-17 27 28 -1 0.0747858 -1 0.0134399 29 40 0.09090907 1 4 93 -0.2876803 2 3 -1 -0.0492811 -1 -0.00753126 33 0.9372537 5 12 151 -0.09782607 6 11 118 0.07067916 7 8 -1 -0.0493046 130 -0.3139535 9 10 -1 -0.120006 -1 0.00793144 -1 0.00185561 201 -0.3124997 13 18 282 15 14 17 199 -3.588234 15 16"
   "-1 0.23392 -1 -0.0276989 -1 0.0357846 194 -4.420315 19 20 -1 -0.0959288 117 0.2495084 21 28 219 -0.4027777 22 23 -1 0.0392912 105 -0.07954544 24 25 -1 -0.0179722 281 221 26 27 -1 0.0200884 -1 -0.255509 -1 -0.0142962 29 188 0.09663133 1 16 156 -0.1818179 2 9 147 0.0293126 3 8 145 -0.9962554 4 5 -1 -0.0372583 110 -1.111111 6 7 -1 0.0679377 -1 0.00129098 -1 -0.0504761 233 -0.2639627 10 11 -1 -0.0135407 15 0.06976744 12 15 15 -7.666662 13 14 -1 0.32278 -1 0.0385811 -1 0.00701346 138 0.8341983 17 26 218 -0.7916664 18 23 216 -0.8890625 19 20 -1 -0.0359923 198 -0.4508928 21 22 -1 0.184801 -1 0.0253094 184 0.007396724 24 25 -1 -0.0458323 -1 -0.0105205 282 37 27 28 -1 0.292817 -1 0.0361044 29 38 0.2833333 1 20 200 93750 2 19 64 -0.125 3 12 3 -0.7066767 4 9 206 -0.2215909 5 8 197 2.041666 6 7 -1 -0.0137201 -1 0.15764 -1 0.0134929 281 36 10 11 -1 -0.00439271 -1 0.0462801 30 0.03104057 13 18 28 0.2845534"
   "14 15 -1 0.0218763 280 0.7837838 16 17 -1 -0.0247364 -1 -0.0899668 -1 0.0142717 -1 -0.0597274 238 -0.3777658 21 22 -1 0.0240377 199 0.2499997 23 24 -1 -0.0378634 99 0.05185569 25 28 94 0.05043324 26 27 -1 0.0249772 -1 0.306508 -1 -0.0384636 29 98 0.876807 1 28 235 0.01023173 2 9 282 133 3 8 96 0.815857 4 7 26 0.2461301 5 6 -1 0.00345503 -1 -0.024263 -1 0.03943 -1 -0.0413386 94 1.077776 10 21 56 0.6871425 11 20 258 0.03570865 12 17 265 -2.035714 13 16 24 0.9617144 14 15 -1 0.0396507 -1 0.261412 -1 -0.00393707 117 -0.1579531 18 19 -1 0.0313021 -1 -0.00134637 -1 -0.0252102 194 0.4892478 22 23 -1 0.0347216 153 0.7499998 24 27 278 7.5 25 26 -1 0.0950573 -1 0.691545 -1 0.00578824 -1 0.0218183 29 25 0.07137809 1 8 280 0.8148148 2 7 114 1.548386 3 6 237 -0.0616429 4 5 -1 -0.0185591 -1 -0.071749 -1 0.146438 -1 0.0208937 98 0.7264113 9 26 13 -0.8333328 10 11 -1 -0.0217705 64 0.1111111 12 23 47 -0.3538212"
   "13 16 228 -0.9318178 14 15 -1 0.0897919 -1 0.00881832 63 -0.4999995 17 18 -1 -0.0423829 27 0.2127361 19 22 56 0.4259062 20 21 -1 0.0823383 -1 -0.041994 -1 -0.00771841 61 -0.1052631 24 25 -1 -0.00889096 -1 0.0230977 99 0.6911368 27 28 -1 -0.0197264 -1 -0.129689 29 27 0.07804405 1 2 -1 -0.0533823 43 0.08948545 3 12 20 -0.2857141 4 7 173 -0.1827957 5 6 -1 0.0204768 -1 -0.0182398 216 0.04596749 8 11 118 0.02472937 9 10 -1 0.043066 -1 0.143226 -1 -0.0218757 24 0.006142598 13 14 -1 -0.0564981 25 0.0004911874 15 20 65 -0.6666666 16 17 -1 -0.0186169 279 12.5 18 19 -1 0.211062 -1 0.0440163 207 -2.067696 21 22 -1 -0.0577272 111 0.8387095 23 28 176 -0.6107784 24 27 131 0.2499998 25 26 -1 0.0027558 -1 0.0480992 -1 -0.00280197 -1 -0.0255006 29 76 0.0445948 1 26 238 -0.3895204 2 5 277 6.5 3 4 -1 0.0281273 -1 -0.0348131 280 0.7916667 6 25 281 97 7 18 54 -0.1734677 8 17 26 0.2892544 9 16 225 -0.642857 10"
   "13 199 -0.02542372 11 12 -1 -0.00533152 -1 -0.172742 75 0.999995 14 15 -1 0.0278454 -1 -0.0219747 -1 0.0647848 -1 -0.00585642 19 -0.4666665 19 24 129 0.8139532 20 23 198 -0.4886362 21 22 -1 -0.0186355 -1 0.0270006 -1 -0.0782826 -1 -0.0262883 -1 -0.0326289 76 2.030592 27 28 -1 -0.0327916 -1 0.116736 29 247 1.999998 1 28 26 0.2350525 2 19 245 0.1770833 3 18 236 0.7499976 4 17 62 -0.09090906 5 8 153 -4.694442 6 7 -1 0.108249 -1 -0.0077532 53 -1.004987 9 10 -1 -0.0292589 157 -1.511904 11 12 -1 0.123712 197 -0.4458333 13 14 -1 0.0484082 151 1.079208 15 16 -1 -0.0018585 -1 0.0800367 -1 0.0313289 -1 0.0690678 33 0.964784 20 23 31 0.9008607 21 22 -1 -0.00810623 -1 0.0173378 156 1.437499 24 25 -1 -0.0287477 190 -0.3960368 26 27 -1 0.356419 -1 0.00376551 -1 0.069118 29 24 0.1203412 1 4 18 -0.999998 2 3 -1 0.0354415 -1 -0.0495414 226 93750 5 28 230 0.0669812 6 17 238 -0.4718209 7 10 237 -0.8037521 8"
   "9 -1 0.0278203 -1 -0.0365025 282 213 11 14 110 0.1818179 12 13 -1 -0.00482715 -1 0.0197245 24 0.821184 15 16 -1 0.0703025 -1 -0.0434993 237 0.6242851 18 27 253 0.136506 19 26 254 -0.1235502 20 23 231 0.2510698 21 22 -1 -0.0190275 -1 -0.068285 72 0.1089563 24 25 -1 -0.0147052 -1 0.0232112 -1 -0.0595584 -1 0.0271435 -1 0.0929104 29 116 0.4818119 1 20 19 -2.399999 2 3 -1 -0.0454715 280 0.03669725 4 11 281 104 5 6 -1 -0.0396236 118 0.04788043 7 10 163 -0.9444232 8 9 -1 -0.031493 -1 0.0783075 -1 -0.0342101 184 0.9492184 12 15 150 1.225806 13 14 -1 0.00186245 -1 -0.0443802 213 -0.4999963 16 17 -1 -0.052562 206 0.7499993 18 19 -1 0.139602 -1 -0.019772 125 -2.730281 21 24 282 73 22 23 -1 0.583547 -1 -0.0349877 181 1.448275 25 26 -1 -0.0420826 220 0.3984374 27 28 -1 -0.0336684 -1 0.407155 29 38 0.6346153 1 26 141 0.3345634 2 23 245 -0.02777777 3 14 277 6.5 4 11 122 -0.4753892 5 10 238 -0.3544396 6"
   "7 -1 0.032761 239 0.4556492 8 9 -1 -0.0106731 -1 0.0371506 -1 -0.0202829 237 -0.1058866 12 13 -1 -0.0761256 -1 -0.00598105 72 -0.6878457 15 16 -1 0.000509029 61 -0.7338129 17 18 -1 0.0558821 198 -0.6874998 19 20 -1 0.055697 188 0.8579456 21 22 -1 -0.00943736 -1 0.0995488 30 0.6286951 24 25 -1 0.0235522 -1 0.215227 26 0.8586217 27 28 -1 -0.010049 -1 -0.0637747 29 267 0.6999994 1 28 45 1.159091 2 27 26 0.8380342 3 10 227 1.708332 4 9 157 -2.857142 5 6 -1 0.0469243 281 75 7 8 -1 0.00916781 -1 -0.00883893 -1 -0.0596594 181 2.885713 11 24 1 0.2736898 12 13 -1 -0.0461971 199 0.0824468 14 15 -1 -0.0242722 3 0.2347115 16 23 42 0.3305785 17 22 202 0.4821427 18 19 -1 0.108975 141 0.09956894 20 21 -1 -0.018913 -1 1.14826 -1 0.038385 -1 0.00528649 154 0.02380952 25 26 -1 0.547151 -1 -0.042092 -1 0.0286712 -1 0.0185194 29 40 0.03846153 1 8 247 -0.06999999 2 3 -1 -0.00737336 158 -1.549999 4 7 129 -0.3749993"
   "5 6 -1 -0.0195287 -1 0.498856 -1 -0.0600342 226 3.374998 9 28 33 0.9372537 10 15 106 -0.1041666 11 12 -1 -0.034864 49 0.09347358 13 14 -1 -0.0162507 -1 0.0147887 201 0.1249998 16 19 282 15 17 18 -1 -0.0172206 -1 0.0203326 18 -0.999996 20 21 -1 0.0719832 77 0.3840705 22 27 223 -0.07499994 23 24 -1 0.00751387 232 -0.8567338 25 26 -1 0.0526315 -1 -0.0265278 -1 0.0257039 -1 0.0824515 29 27 0.07804405 1 2 -1 -0.0510048 34 0 3 10 25 0 4 7 76 -0.5278333 5 6 -1 0.15276 -1 -0.0889994 228 -0.6630434 8 9 -1 -0.0438457 -1 0.0285164 58 0.4999998 11 28 207 -2.904851 12 13 -1 -0.0724419 240 0.009969269 14 23 195 0.1216216 15 18 168 1.138015 16 17 -1 -0.00116729 -1 0.0878495 219 -2.142856 19 22 40 0.1818181 20 21 -1 -0.343239 -1 0.0025047 -1 0.0250258 38 0.02127659 24 27 23 0.6074081 25 26 -1 -0.0493781 -1 0.126758 -1 -0.00111795 -1 -0.016801 29 48 0.08599712 1 22 153 0.15625 2 21 245 -0.08552631 3 6 277"
   "5.5 4 5 -1 -0.00313516 -1 -0.0431328 256 0.06884614 7 20 145 -0.01872478 8 19 105 -0.4444444 9 12 38 0.35 10 11 -1 0.0139635 -1 -0.0502675 184 -0.705465 13 16 73 -1.264756 14 15 -1 0.0909773 -1 -0.0507602 111 1.812499 17 18 -1 -0.0113861 -1 0.0755994 -1 0.0343651 -1 0.0178329 -1 0.0210613 5 -1.668421 23 24 -1 0.116683 139 0.5575754 25 26 -1 -0.0458344 194 0.3301908 27 28 -1 -0.00354521 -1 0.234565 29 218 -0.4999998 1 8 27 1.111161 2 5 221 2.124992 3 4 -1 -0.020603 -1 0.152531 282 97 6 7 -1 0.20082 -1 -0.00475366 241 -1.216666 9 12 178 0.2105261 10 11 -1 0.00788248 -1 0.0666798 12 1.230768 13 26 100 -1.091415 14 21 126 -0.2333333 15 20 221 0.1944444 16 17 -1 0.00854664 182 0.122807 18 19 -1 0.146078 -1 0.0164684 -1 -0.000302278 7 -7.946742 22 23 -1 -0.112892 206 1.410713 24 25 -1 -0.000775454 -1 -0.0860219 37 1.318182 27 28 -1 0.0162077 -1 0.0890492 29 25 0.2011267 1 14 233 -0.2639627 2 7"
   "277 4.5 3 6 261 -0.3952391 4 5 -1 0.0480388 -1 -0.0341089 -1 0.0393567 216 -0.6440723 8 13 200 1.656249 9 12 24 0.3280498 10 11 -1 -0.0573634 -1 0.0901138 -1 0.264469 -1 -0.0536584 202 0.6874997 15 26 282 7 16 17 -1 -0.0433609 168 -0.08068563 18 23 195 0.69863 19 20 -1 -0.00804823 49 -0.7482396 21 22 -1 0.154521 -1 0.0127939 131 0.06249998 24 25 -1 0.00103709 -1 0.0234707 5 0.09055565 27 28 -1 -0.0422719 -1 0.0102694 29 23 0.9827959 1 28 254 0.03998705 2 27 260 -0.02423247 3 6 233 -0.08262968 4 5 -1 -0.022313 -1 0.00647881 38 0.02127659 7 14 281 20 8 13 134 -0.6666664 9 10 -1 -0.0278466 29 0.2829482 11 12 -1 -0.0641956 -1 0.114642 -1 -0.0323288 176 0.4615382 15 26 117 -0.9087032 16 19 124 -0.6656991 17 18 -1 -0.0161673 -1 0.0678218 56 0.799202 20 25 71 0.2887632 21 24 24 0.01693331 22 23 -1 0.121638 -1 0.00360439 -1 0.0385754 -1 0.0676022 -1 -0.0183291 -1 -0.00766743 -1 -0.0318322 29 38 0.2833333"
   "1 8 266 -0.5571427 2 5 243 0.02272726 3 4 -1 -0.0555742 -1 0.00109125 132 0.1428571 6 7 -1 0.0117324 -1 -0.00990405 281 107 9 28 282 63 10 23 5 0.2458711 11 22 26 0.1603645 12 13 -1 -0.0351756 28 0.2904891 14 17 29 0.4930789 15 16 -1 0.307869 -1 0.0756055 281 60 18 21 279 12.5 19 20 -1 0.162903 -1 0.0329559 -1 0.00802235 -1 -0.0340571 222 -4.861109 24 27 282 107 25 26 -1 0.597823 -1 0.0235856 -1 -0.0306322 -1 -0.0452205 29 270 -1.699999 1 4 0 -0.4512425 2 3 -1 -0.0182513 -1 0.0934091 26 0.2461301 5 8 245 0.06382978 6 7 -1 0.0040552 -1 0.0385675 32 0.4294311 9 10 -1 -0.0380289 176 0.2884614 11 24 223 0.4642856 12 15 33 0.9488894 13 14 -1 0.00967416 -1 -0.0185095 153 0.06521739 16 17 -1 -0.0359304 170 0.4246445 18 19 -1 -0.0202832 42 0.08474575 20 23 207 -0.08884143 21 22 -1 0.655661 -1 0.0180038 -1 0.0385821 123 1.265574 25 28 168 -0.2873045 26 27 -1 0.18691 -1 0.0147364 -1 0.304976 29 40"
   "0.03846153 1 8 37 0.4146341 2 3 -1 -0.0452728 204 -1.575757 4 5 -1 0.36521 109 -0.05555553 6 7 -1 -0.0302153 -1 0.0329278 238 0.1711559 9 22 141 0.150343 10 15 207 0.3587271 11 14 282 16 12 13 -1 -0.0177015 -1 0.0195203 -1 -0.0147816 49 0.2407789 16 19 232 -0.9296685 17 18 -1 0.0343913 -1 -0.0349421 71 1.187252 20 21 -1 0.023404 -1 -0.0938746 237 0.07371289 23 26 3 -4.068119 24 25 -1 0.117852 -1 -0.0256607 26 0.754983 27 28 -1 -0.029992 -1 0.0280289 29 26 1.020031 1 28 188 0.09663133 2 25 232 1.049069 3 24 195 -0.1785713 4 19 204 0.1979166 5 18 135 -0.361111 6 15 197 -0.310606 7 12 102 0.4673278 8 11 87 -0.4999998 9 10 -1 -0.0171137 -1 -0.0989504 -1 0.00399412 102 0.7306325 13 14 -1 0.044297 -1 -0.0833904 212 0.2642337 16 17 -1 0.0239289 -1 -0.0192926 -1 -0.0494693 130 0.5106383 20 21 -1 0.0155891 125 0.02877899 22 23 -1 -0.0755246 -1 0.001416 -1 -0.0531913 197 -0.0125 26 27 -1 -0.0285972"
   "-1 -0.000673888 -1 0.0607334 29 280 0.7837838 1 24 281 97 2 15 282 9 3 6 172 -0.09999996 4 5 -1 0.0117764 -1 0.0859996 85 -0.1052631 7 8 -1 0.020093 197 0.8391812 9 14 134 0.6153844 10 13 221 -0.6315788 11 12 -1 -0.0805219 -1 -0.00413641 -1 0.0331213 -1 -0.0663656 19 -0.4666665 16 17 -1 0.0054136 282 127 18 23 8 -1.377391 19 20 -1 0.0736518 197 1.567307 21 22 -1 -0.0360228 -1 0.0893253 -1 0.0537416 31 0.03585743 25 26 -1 0.0617213 48 -0.9085873 27 28 -1 0.000431329 -1 -0.0466027 29 266 0.5428571 1 28 157 -1.165517 2 9 99 -1.003297 3 8 226 0.6224488 4 7 223 0.4791663 5 6 -1 0.00557506 -1 -0.0707754 -1 0.129016 -1 0.038014 210 0.1336385 10 19 282 130 11 16 96 1.433113 12 15 102 -2.003785 13 14 -1 -0.0521448 -1 -0.00476104 -1 0.10524 151 1.134615 17 18 -1 -0.0492984 -1 0.0553203 113 0.08823527 20 25 217 0.08543131 21 24 198 -0.195 22 23 -1 0.0283619 -1 -0.0144623 -1 -0.0395151 121 1.479441 26"
   "27 -1 0.0199453 -1 0.170568 -1 0.0150854 29 11 -1.444444 1 2 -1 -0.039182 281 115 3 24 282 69 4 9 98 0.1999992 5 8 41 0.3968253 6 7 -1 -0.0338081 -1 0.00988181 -1 -0.0441775 203 0.3999998 10 19 120 0.1305677 11 18 197 0.8708332 12 15 189 1.232196 13 14 -1 -0.00595527 -1 -0.180105 24 0.778299 16 17 -1 -0.0228914 -1 -0.217605 -1 0.016496 140 0.9229379 20 23 208 -0.3162779 21 22 -1 0.0616474 -1 0.0092166 -1 -0.074417 195 0.004504504 25 26 -1 0.027173 206 -1.468749 27 28 -1 -0.116961 -1 -0.00249067 29 26 0.00025822 1 2 -1 -0.0571771 25 0 3 8 65 -0.6666666 4 5 -1 -0.0411374 79 0.9802161 6 7 -1 0.121592 -1 -0.0789312 43 0.6428571 9 26 263 -0.01630428 10 17 119 -0.2356028 11 16 282 110 12 15 188 -0.5581903 13 14 -1 -0.0119301 -1 -0.0469984 -1 0.00768087 -1 0.0099907 33 0.9158995 18 19 -1 -0.0108333 45 0.7499999 20 25 282 7 21 22 -1 0.0519735 184 -0.03712667 23 24 -1 -0.0052188 -1 0.0259313 -1 0.0461351"
   "158 -0.1975308 27 28 -1 0.0125548 -1 -0.0160282 29 122 0.05842985 1 26 42 1.205821 2 25 34 0.7835051 3 16 191 0.1702214 4 15 226 0.7451922 5 14 136 -1.338983 6 11 200 0.9583321 7 10 181 -1.006211 8 9 -1 -0.110297 -1 0.00327998 -1 -0.157265 186 -1.351333 12 13 -1 0.0417806 -1 0.00350582 -1 -0.0334557 -1 0.0274277 33 0.7585728 17 18 -1 0.0554708 193 -1.634974 19 20 -1 0.123749 38 0.1555555 21 24 134 0.3541666 22 23 -1 -0.0231322 -1 0.0259893 -1 -0.0399709 -1 0.0430212 72 -0.3504203 27 28 -1 0.0186053 -1 -0.036137 29 203 16.37493 1 26 266 0.4533332 2 25 197 -0.827586 3 4 -1 0.0189386 282 100 5 22 281 123 6 15 217 0.6721017 7 10 96 0.8445125 8 9 -1 0.00223291 -1 0.0486518 206 0.2968749 11 12 -1 0.0168518 56 0.3848858 13 14 -1 0.0860526 -1 0.523578 119 0.2135254 16 17 -1 -0.032973 105 -0.09259257 18 21 210 -1.21684 19 20 -1 0.267146 -1 0.0503817 -1 -0.020682 151 0.7792206 23 24 -1 -0.0251654 -1"
   "0.0322009 -1 0.014392 193 -0.5023862 27 28 -1 0.150503 -1 -0.100094 29 60 -0.3333332 1 12 40 0.03125 2 3 -1 -0.0469713 27 0.3957908 4 9 53 0.262123 5 8 148 0.07425213 6 7 -1 -0.00818964 -1 0.0555367 -1 -0.00888373 28 0.3782829 10 11 -1 -0.0693075 -1 -0.0119081 98 0.876807 13 26 87 -0.999996 14 15 -1 0.0536205 61 0.02083333 16 25 191 0.133212 17 24 130 -0.494186 18 23 229 -0.4249998 19 20 -1 0.0485408 127 -0.9336734 21 22 -1 0.0819216 -1 -0.0373825 -1 0.0133353 -1 -0.0341455 -1 0.0127391 134 -2.562499 27 28 -1 0.206888 -1 -0.0370966 29 8 -2.310031 1 2 -1 -0.0769393 150 1.225806 3 28 241 0.5113635 4 27 34 0.9411764 5 22 206 1.609374 6 17 26 1.010926 7 8 -1 -0.000730119 184 -0.1073596 9 12 3 0.4780563 10 11 -1 -0.0800906 -1 0.0693525 28 0.6439099 13 16 4 0.1327053 14 15 -1 0.092683 -1 0.280729 -1 -0.00326518 123 0.4281741 18 19 -1 0.0349276 0 0.1735266 20 21 -1 0.364837 -1 -0.0230153 92 0.1621141"
   "23 26 150 0.6249996 24 25 -1 0.0301522 -1 -0.0357906 -1 -0.0288592 -1 -0.0120404 -1 -0.0363833 29 48 0.1108029 1 26 141 0.3023856 2 25 94 -1.396325 3 8 199 -4.349998 4 7 149 -5.70833 5 6 -1 0.123292 -1 -0.100524 -1 0.0258666 200 62500 9 24 125 -0.7824329 10 11 -1 0.0111032 282 8 12 15 281 105 13 14 -1 0.0531153 -1 -0.0205265 196 -1.145833 16 19 37 0.1052631 17 18 -1 0.236406 -1 0.029429 195 -1.468749 20 21 -1 -0.0747092 227 0.1249999 22 23 -1 -0.024152 -1 -0.000649843 -1 -0.0636183 -1 0.0268401 5 -1.668421 27 28 -1 0.0994926 -1 -0.0337687 29 175 2.249998 1 26 218 -0.4999998 2 17 27 1.111161 3 14 141 -1.389687 4 5 -1 0.0345193 255 0.2765974 6 11 108 1.249998 7 8 -1 -0.0392752 188 -0.9624 9 10 -1 0.31944 -1 -0.00256747 281 121 12 13 -1 0.0350616 -1 -0.0621952 282 97 15 16 -1 0.163713 -1 -0.00846391 156 0.1818181 18 25 125 0.6200529 19 24 94 1.077776 20 21 -1 0.00957877 194 0.4892478 22 23 -1"
   "0.0332965 -1 0.270616 -1 -0.030627 -1 -0.00676439 107 0.06249994 27 28 -1 0.0313338 -1 0.232329 29 208 0.6432048 1 26 164 -0.0356665 2 11 282 216 3 10 59 -0.6249998 4 5 -1 -0.0378637 112 1.859375 6 9 119 -0.01361891 7 8 -1 0.00320238 -1 -0.0191422 -1 0.111051 -1 0.0492627 233 0.6698443 12 25 281 115 13 22 92 0.07681477 14 21 219 -0.3846153 15 20 168 0.2720557 16 19 34 0.5882352 17 18 -1 -0.040359 -1 0.0894149 -1 -0.0276434 -1 -0.0333515 -1 0.011541 206 -1.468749 23 24 -1 -0.0893198 -1 0.0235647 -1 0.0482002 224 0.3333333 27 28 -1 -0.0796479 -1 -0.00545454 29 207 -2.904851 1 2 -1 -0.0579336 24 0.0007453508 3 4 -1 -0.049516 25 0 5 10 19 -0.6666665 6 7 -1 -0.0307288 110 0.03749999 8 9 -1 0.140314 -1 0.00303833 228 0.2272726 11 22 95 0.3812123 12 19 145 0.1692767 13 14 -1 0.000301617 24 0.7366881 15 16 -1 0.00808767 138 0.1144455 17 18 -1 0.0294082 -1 0.102862 185 0.9254523 20 21 -1 -0.0377619"
   "-1 0.120573 281 187 23 24 -1 -0.0120315 198 -0.02499998 25 26 -1 -0.00690873 3 0.02529278 27 28 -1 0.0427615 -1 0.258413 29 280 0.7837838 1 26 281 97 2 15 54 -0.1734677 3 12 229 -0.4130434 4 7 131 -0.01923076 5 6 -1 0.0196634 -1 -0.0883339 26 0.1603645 8 9 -1 0.0138626 123 -0.4918377 10 11 -1 0.0861306 -1 0.0325429 86 0.1538461 13 14 -1 0.00607035 -1 -0.028048 38 0.26 16 21 3 -0.7066767 17 18 -1 -0.0101853 76 -0.05947784 19 20 -1 0.0467062 -1 -0.0249435 223 -0.8214281 22 25 218 -3.916661 23 24 -1 0.459195 -1 -0.0352262 -1 -0.0376948 31 0.03585743 27 28 -1 0.0579375 -1 -0.030667 29 247 0.9166661 1 26 26 0.2350525 2 5 245 0.1770833 3 4 -1 0.0048639 -1 0.0550009 12 0.4999998 6 19 270 0.7999994 7 16 156 1.979166 8 15 186 1.156168 9 12 32 0.433766 10 11 -1 -0.0514295 -1 -0.0166516 26 0.8109191 13 14 -1 0.109474 -1 -0.0590936 -1 0.147902 240 0.3224322 17 18 -1 -0.00618783 -1 0.0676632 267 -2.533333"
   "20 21 -1 0.165966 58 0.3333331 22 23 -1 -0.0146593 244 -0.1818181 24 25 -1 -0.0137784 -1 0.03743 277 6.5 27 28 -1 0.0138314 -1 0.0905411 29 134 562500 1 28 25 0.2340678 2 13 232 -0.07069274 3 8 277 5.5 4 5 -1 -0.0148545 155 -0.21 6 7 -1 -0.0562859 -1 0.0516218 211 -0.7146363 9 12 200 1.447916 10 11 -1 -0.00770995 -1 0.237738 -1 -0.0524328 201 0.1406249 14 25 41 0 15 20 248 -0.02272726 16 17 -1 0.011322 221 -1.349999 18 19 -1 0.155377 -1 -0.0573239 83 -0.4999998 21 22 -1 -0.00839666 281 91 23 24 -1 0.00886889 -1 0.0347948 206 0.6180554 26 27 -1 -0.0110934 -1 0.024902 -1 0.102405 29 271 0.06249997 1 28 41 0.04545454 2 27 38 0.03333333 3 10 188 0.05517992 4 9 111 -0.4999999 5 6 -1 -0.0475688 170 0.970951 7 8 -1 0.0200265 -1 0.280484 -1 -0.0729579 282 198 11 20 79 0.1804478 12 19 79 0.08695512 13 14 -1 0.0307259 93 -0.2010862 15 16 -1 0.0373863 142 -0.3903883 17 18 -1 -0.254563 -1 -0.0133643"
   "-1 0.0613174 28 0.5848789 21 26 225 -0.4999995 22 25 109 -0.4687498 23 24 -1 0.0702997 -1 -0.113436 -1 -0.181771 -1 0.0697572 -1 -7.63753e-05 -1 -0.0067874 29 280 0.7837838 1 24 281 97 2 17 282 9 3 6 180 -0.04545453 4 5 -1 0.00251375 -1 0.0751656 85 -0.1052631 7 8 -1 0.0173134 197 0.8391812 9 16 88 0.9999996 10 15 221 -0.6315788 11 14 131 -0.05309734 12 13 -1 0.0855809 -1 -0.0964815 -1 -0.00310644 -1 0.0378311 -1 -0.0619817 65 -0.4999998 18 19 -1 0.00657115 282 127 20 23 123 -1.412859 21 22 -1 0.0756684 -1 -0.0275611 -1 0.050577 31 0.03585743 25 28 114 -1.456521 26 27 -1 -0.130141 -1 0.0755645 -1 -0.0286493 29 258 0.03925923 1 12 31 1.017587 2 7 282 97 3 4 -1 -0.00240995 201 -0.7499993 5 6 -1 0.0308895 -1 -0.0349303 163 0.09990214 8 11 88 -2.999999 9 10 -1 0.157023 -1 -0.000653527 -1 0.057952 100 -1.1188 13 16 200 -0.6527775 14 15 -1 -0.0432122 -1 0.0449453 98 -0.2916641 17 20 155 -0.4999998"
   "18 19 -1 0.0121889 -1 -0.0296651 280 0.07317073 21 22 -1 -0.0759768 110 0.7499995 23 28 185 0.5034492 24 27 212 -0.5726826 25 26 -1 0.0662292 -1 0.014219 -1 -0.0257641 -1 -0.0303222 29 207 0.3587271 1 26 138 0.7454207 2 23 100 0.1914133 3 14 32 0.7234127 4 11 40 0.04347825 5 6 -1 -0.0242624 184 -0.0101836 7 10 282 7 8 9 -1 -0.0355015 -1 0.0187202 -1 -0.00582306 37 0.590909 12 13 -1 -0.0408408 -1 -0.00281728 282 45 15 16 -1 -0.0190967 176 -3.692307 17 20 190 0.4829519 18 19 -1 0.000607118 -1 -0.382714 263 -1.358298 21 22 -1 0.0823134 -1 0.0229202 282 37 24 25 -1 -0.164222 -1 -0.0316829 118 0.03656924 27 28 -1 -0.036778 -1 0.00479951 29 6 -2.052969 1 4 59 -1.071428 2 3 -1 0.0144081 -1 -0.0818884 39 0 5 24 38 0.02127659 6 17 280 0.9090909 7 16 61 -0.0338983 8 13 281 144 9 10 -1 -0.0586525 204 -0.7333331 11 12 -1 -0.07346 -1 0.0526593 190 0.07870312 14 15 -1 0.117819 -1 -0.0618617 -1 0.040599"
   "63 -0.7777774 18 19 -1 0.0475319 151 0.3116882 20 21 -1 0.00130181 206 1.609374 22 23 -1 0.0297905 -1 0.237756 139 -2.854688 25 26 -1 0.100155 138 -1.073063 27 28 -1 -0.0358409 -1 -0.00162218 29 42 0 1 4 10 -1.897331 2 3 -1 0.014292 -1 -0.0678935 229 -0.4130434 5 10 174 -0.125 6 7 -1 -0.00250743 41 0.09999998 8 9 -1 -0.00374368 -1 -0.0732258 280 0.6116505 11 28 281 96 12 13 -1 0.0221654 74 -1.424569 14 15 -1 0.0515563 33 0.8359178 16 17 -1 0.0199475 0 -2.240327 18 19 -1 0.064078 24 0.0007453508 20 23 187 -1.109465 21 22 -1 -0.108118 -1 0.105627 202 -0.499999 24 25 -1 -0.061276 282 127 26 27 -1 -0.0187326 -1 0.0637983 -1 -0.00928036 29 79 -4.74587 1 4 101 1.688481 2 3 -1 -0.0325283 -1 0.103099 259 -1.439646 5 6 -1 0.0412668 157 -1.165517 7 18 181 -0.8333328 8 11 29 0.8829776 9 10 -1 0.049508 -1 0.291659 129 -0.6666662 12 13 -1 -0.0328493 164 -0.9117349 14 17 2 -4.931815 15 16 -1 -0.057804"
   "-1 0.0837255 -1 0.0131995 273 2.187498 19 28 218 -0.4772726 20 23 281 89 21 22 -1 0.00114589 -1 -0.0475516 228 -0.7916664 24 25 -1 -0.0497465 37 1.272727 26 27 -1 0.00118665 -1 0.0456772 -1 0.0914478 29 11 -1.444444 1 2 -1 -0.0342454 98 0.9499924 3 24 134 562500 4 23 200 218750 5 22 60 -0.3333332 6 13 27 0.3957908 7 10 227 -1.714285 8 9 -1 0.150877 -1 0.00442867 28 0.3497242 11 12 -1 -0.0697765 -1 -0.013823 87 -0.999996 14 15 -1 0.0514388 61 0.02083333 16 21 191 0.19674 17 20 210 0.3723576 18 19 -1 0.00514574 -1 -0.0430924 -1 -0.035853 -1 0.011093 -1 0.0859543 -1 0.100866 107 0.3510638 25 26 -1 -0.0193443 136 -0.1012658 27 28 -1 0.0108639 -1 -0.182164 29 193 -0.03439538 1 22 202 -0.175 2 3 -1 -0.00464094 200 -0.6527775 4 5 -1 0.108628 124 0.4821088 6 15 163 0.6968346 7 8 -1 0.0111932 144 0.9017806 9 12 137 -0.6774192 10 11 -1 0.0444704 -1 -0.0771115 133 0.3999987 13 14 -1 -0.0138928 -1 0.451216"
   "203 0.05208332 16 17 -1 0.00333645 229 -0.5652173 18 19 -1 0.323203 280 0.5211268 20 21 -1 0.140261 -1 0.00193018 185 0.05575159 23 26 128 -1.095238 24 25 -1 0.037141 -1 -0.0306279 281 221 27 28 -1 0.000706031 -1 0.268897 29 19 2.166665 1 28 34 0.7835051 2 21 42 0 3 6 19 -0.81203 4 5 -1 0.0214597 -1 -0.0593461 136 -1.338983 7 10 131 -0.2343749 8 9 -1 -0.00481519 -1 -0.0835708 37 0.15 11 12 -1 0.0253129 23 0.1506911 13 14 -1 -0.0261458 105 0.07317072 15 20 133 0.458333 16 19 34 0.3333332 17 18 -1 0.0270391 -1 -0.000966094 -1 0.0460646 -1 -0.00818219 11 0.4166665 22 23 -1 -0.0405613 38 0.2264151 24 27 10 1.11003 25 26 -1 0.00753159 -1 0.131 -1 -0.0253749 -1 0.0356389 29 243 1.277777 1 28 247 1.999998 2 27 23 0.200185 3 20 121 -0.9890058 4 5 -1 -0.0274098 132 -0.5288461 6 13 196 -0.3902438 7 10 205 -0.6071425 8 9 -1 0.0363637 -1 0.161304 45 0.1999999 11 12 -1 0.0681848 -1 -0.0233083 241 -1.166665"
   "14 15 -1 0.0736366 136 -1.141304 16 19 229 0.05921051 17 18 -1 -0.0491083 -1 0.257297 -1 -0.00481454 32 0.4294311 21 22 -1 -0.0329567 146 0.1692024 23 24 -1 0.00645005 28 1.039478 25 26 -1 -0.0172366 -1 0.0231312 -1 0.0652339 -1 0.0324028 29 14 -0.3333333 1 8 108 -0.4999995 2 3 -1 -0.0521899 33 0.9913783 4 5 -1 -0.0192749 139 -0.03790106 6 7 -1 0.0220831 -1 -0.018364 137 -0.6617646 9 12 37 0.1333333 10 11 -1 0.333949 -1 0.0329472 146 -0.09188578 13 20 84 0.06185567 14 15 -1 -0.0304594 26 0.5611015 16 17 -1 -0.0457821 155 0.3124997 18 19 -1 0.0229385 -1 -0.020563 138 0.6486447 21 24 106 -0.08108105 22 23 -1 -0.0173377 -1 0.0191934 282 59 25 26 -1 -0.0946224 28 0.4685347 27 28 -1 -0.112908 -1 0.00148536 29 23 0.9843697 1 28 4 0.1603859 2 9 212 0.3258056 3 8 263 -0.01630428 4 7 10 -5.284679 5 6 -1 0.10637 -1 -0.00838753 -1 0.00904258 -1 -0.0206793 33 0.802777 10 11 -1 -0.0284461 129 0.1529411"
   "12 23 26 1.005785 13 22 218 -0.3083333 14 21 171 -0.6425304 15 16 -1 0.0139653 124 0.1115933 17 18 -1 0.0285994 40 0.2999999 19 20 -1 0.0384958 -1 0.177778 -1 0.0157362 -1 0.168861 8 1.160778 24 25 -1 -0.00165219 281 73 26 27 -1 -0.0630057 -1 0.166945 -1 -0.0291501 29 195 -5.425923 1 2 -1 0.0932736 199 -4.349998 3 4 -1 -0.0850684 186 -1.351333 5 6 -1 0.0357423 187 -0.8875519 7 10 191 -0.6160929 8 9 -1 -0.0278762 -1 0.0259938 19 2.166665 11 28 37 0.4210526 12 23 282 226 13 20 34 0.8095237 14 15 -1 0.0162548 189 -1.077146 16 19 281 198 17 18 -1 0.0502445 -1 0.379988 -1 -0.0222931 192 -0.4504188 21 22 -1 -0.195444 -1 -0.0232196 34 0 24 25 -1 -0.0654467 40 0.2857142 26 27 -1 0.004897 -1 -0.0152647 -1 0.0453068 29 203 16.37493 1 26 2 -0.4094949 2 5 15 -0.6249999 3 4 -1 0.000598442 -1 0.031614 96 -0.04168473 6 11 230 0.1887881 7 8 -1 -0.0348012 166 -0.7467973 9 10 -1 0.128893 -1 -0.00631979 200"
   "-0.7499993 12 17 187 -0.7716734 13 14 -1 0.0355932 223 0.2249999 15 16 -1 0.642238 -1 0.0529352 112 -0.1860464 18 25 122 -0.06200595 19 24 200 0.1249999 20 21 -1 0.0277279 150 -0.02941175 22 23 -1 0.218884 -1 -0.000435418 -1 -0.000963835 -1 -0.00314846 188 -0.6908639 27 28 -1 0.148997 -1 -0.0943536 29 208 0.6432048 1 26 164 -0.0356665 2 23 282 216 3 22 31 0.3042945 4 5 -1 -0.0375361 112 1.859375 6 21 119 0.0263284 7 8 -1 0.00356828 282 45 9 10 -1 -0.0617986 143 -0.1491795 11 14 282 143 12 13 -1 -0.0704066 -1 0.116386 223 -0.8749965 15 20 218 -1.53125 16 19 1 0.2067081 17 18 -1 -0.0795014 -1 -0.54497 -1 0.0137757 -1 0.00853226 -1 0.108911 -1 0.0444405 233 0.6698443 24 25 -1 0.00418377 -1 0.046063 133 0.2499997 27 28 -1 -0.0646066 -1 0.00381552 29 70 -1.592347 1 14 227 0.8854164 2 11 186 -6.449373 3 4 -1 0.131687 148 0.09583113 5 10 198 0.6874986 6 7 -1 -0.077703 82 -0.1666666 8 9 -1 0.156096"
   "-1 -0.0710893 -1 -0.00347958 156 0.1547619 12 13 -1 -0.0397892 -1 0.152138 179 -0.9999996 15 18 141 -0.03528931 16 17 -1 0.0701876 -1 0.00194905 197 -3.599999 19 20 -1 -0.0984259 209 0.6605638 21 22 -1 0.00210218 141 0.250307 23 26 56 0.7170366 24 25 -1 -0.0501398 -1 0.0349737 214 0.9052851 27 28 -1 -0.0338045 -1 0.0939999 29 122 0.05842985 1 22 153 0.15625 2 17 136 -1.338983 3 4 -1 -0.0323892 125 -0.4883479 5 14 204 0.2999998 6 13 136 -0.4249998 7 8 -1 0.0261666 133 0.02272726 9 12 134 0.1971154 10 11 -1 -0.038016 -1 0.0120798 -1 0.0149987 -1 0.0266796 97 -0.08900306 15 16 -1 0.00140674 -1 -0.0220055 278 3.5 18 21 280 0.2111111 19 20 -1 0.0951657 -1 0.0272677 -1 0.00584754 49 -0.2472315 23 28 168 0.8034515 24 27 136 0.517857 25 26 -1 -0.0252708 -1 0.0603567 -1 0.0518463 -1 -0.0444384 29 179 -0.7777774 1 2 -1 -0.0239017 100 -1.1188 3 4 -1 0.0264306 52 -0.2666645 5 8 132 -0.4999997 6 7 -1"
   "0.0134365 -1 -0.018039 129 0.8499993 9 28 257 -0.8745942 10 13 183 -2.775558e-17 11 12 -1 0.0805697 -1 0.00262754 116 0.4179668 14 27 281 75 15 22 138 -0.6621105 16 21 64 -0.9999992 17 18 -1 -0.100631 151 -0.313253 19 20 -1 0.0142646 -1 0.0840288 -1 -0.00172118 152 -0.1363636 23 26 193 -0.006614448 24 25 -1 -0.00896181 -1 0.0378737 -1 -0.0257376 -1 0.0276418 -1 0.0321232 29 63 -0.09090906 1 18 24 0.5228494 2 13 152 0.1666666 3 6 181 -1.722221 4 5 -1 0.133941 -1 -0.0401223 3 -0.7185753 7 12 56 -0.1184255 8 9 -1 -0.0191394 278 5.5 10 11 -1 -0.00844215 -1 0.079415 -1 -0.0372024 155 0.2499997 14 17 254 0.03711055 15 16 -1 -0.00255983 -1 0.0270133 -1 -0.0253995 59 -0.736842 19 20 -1 -0.029901 18 -0.9642854 21 22 -1 0.0661857 250 0.9687495 23 28 61 0.02083333 24 27 36 0.5194805 25 26 -1 -0.028333 -1 0.00587353 -1 0.0162393 -1 -0.0304963 29 246 -0.07499997 1 2 -1 -0.00648288 245 0.9536082 3 28 39"
   "0.5714282 4 21 25 0.09317186 5 6 -1 -0.0110848 140 0.7687792 7 18 145 0.2397863 8 17 32 0.6431721 9 10 -1 0.00486821 23 0.9271116 11 12 -1 0.0519465 191 0.1374964 13 14 -1 -0.0631319 198 0.2031249 15 16 -1 -0.0353992 -1 0.308526 -1 0.0486931 56 0.1379613 19 20 -1 -0.0665238 -1 0.0135373 176 -0.3142857 22 23 -1 0.0148873 145 0.7217784 24 25 -1 -0.0190182 139 0.468045 26 27 -1 0.0777811 -1 -0.0629595 -1 -0.0346362 29 48 0.1108029 1 24 153 0.15625 2 19 241 -0.2499998 3 4 -1 -0.0136805 107 -0.7342995 5 12 93 -0.459634 6 7 -1 0.0067717 223 0.8124984 8 11 54 0.8680809 9 10 -1 0.0558708 -1 -0.0715191 -1 0.147417 106 0.08333328 13 18 125 -0.4461638 14 15 -1 0.032063 191 -0.7425928 16 17 -1 -0.0662675 -1 0.00140988 -1 -0.0247089 25 0.9678422 20 21 -1 0.0157115 85 -0.3749998 22 23 -1 0.386452 -1 -0.056038 250 -0.6999994 25 26 -1 0.0382078 5 -1.668421 27 28 -1 0.0869953 -1 -0.0400734 29 268 -0.1304348"
   "1 4 282 122 2 3 -1 0.020507 -1 -0.027407 30 0.09177791 5 16 168 0.1154569 6 15 95 -0.6854885 7 10 125 -0.9727261 8 9 -1 -0.00955844 -1 0.0177754 64 -0.2857141 11 12 -1 -0.0374876 197 -0.7749994 13 14 -1 0.0556578 -1 -0.00905483 -1 0.0210707 7 -0.3940392 17 24 3 -0.5246672 18 21 220 1.153846 19 20 -1 -0.0154655 -1 0.132309 102 -0.1988775 22 23 -1 0.00111795 -1 0.0701428 144 -0.2115372 25 28 94 1.077776 26 27 -1 -0.0580486 -1 0.085176 -1 -0.0115287 29 226 93750 1 28 281 114 2 9 154 -0.2650602 3 6 182 -0.9041094 4 5 -1 0.037043 -1 -0.0395198 282 194 7 8 -1 -0.00481053 -1 0.0238735 195 0.004504504 10 19 10 0.04410764 11 14 31 0.570795 12 13 -1 -0.0423529 -1 0.0223447 115 -0.6015828 15 16 -1 -0.0108686 135 -0.5714282 17 18 -1 0.096261 -1 0.0361922 151 -2.147058 20 21 -1 0.134752 67 -0.4999999 22 23 -1 -0.0511777 281 221 24 25 -1 0.00480321 168 0.1467235 26 27 -1 -0.185608 -1 0.0423627 -1 0.07128"
   "29 27 0.07804405 1 2 -1 -0.0476128 150 1.225806 3 28 258 -0.6512793 4 5 -1 -0.0213174 138 0.9517897 6 27 116 0.4261546 7 22 34 0 8 17 37 0.3499999 9 10 -1 -0.00123919 27 0.3623694 11 12 -1 0.127692 25 0.149626 13 16 15 -0.8510637 14 15 -1 -0.00537085 -1 0.179684 -1 0.0118612 1 -0.06605526 18 19 -1 -0.00271292 83 -0.25 20 21 -1 0.0321603 -1 0.000558851 139 0.8562435 23 24 -1 -0.0348407 102 -1.354982 25 26 -1 0.464172 -1 0.0275751 -1 0.0621569 -1 -0.0354294 29 5 -1.668421 1 4 159 -1.374999 2 3 -1 -0.053364 -1 0.0423972 42 0 5 8 19 -0.81203 6 7 -1 0.0167094 -1 -0.0623218 48 0.1108029 9 28 141 0.3023856 10 27 245 -0.02777777 11 24 277 6.5 12 21 151 -0.722689 13 14 -1 -0.0309918 123 -0.08412721 15 20 135 0.1249999 16 19 145 0.2134984 17 18 -1 -0.00777125 -1 0.052557 -1 0.0446575 -1 -0.0111576 237 -0.2184903 22 23 -1 -0.0711904 -1 -0.00584276 256 0.0505227 25 26 -1 -0.00113456 -1 0.0189379 -1 0.0282139"
   "-1 -0.0264937 29 258 0.03925923 1 18 31 1.017587 2 13 198 -0.14 3 10 4 -0.4147805 4 5 -1 -0.030576 23 0.1353793 6 9 89 -0.1428571 7 8 -1 0.299522 -1 0.00621351 -1 0.0112304 281 143 11 12 -1 -0.0144051 -1 -0.0590678 163 0.09990214 14 17 88 -2.999999 15 16 -1 0.143138 -1 -0.00251458 -1 0.0507527 277 4.5 19 22 18 0.4583332 20 21 -1 0.0188811 -1 -0.0162819 101 -0.9693676 23 24 -1 0.0369729 281 74 25 26 -1 0.00884568 24 0.986012 27 28 -1 -0.0216789 -1 0.142476 29 231 -0.582323 1 6 277 2.5 2 5 254 0.5354427 3 4 -1 -0.0351213 -1 0.0522933 -1 0.0383538 26 0.4763935 7 16 216 -0.6928078 8 13 117 -0.4629542 9 12 5 -0.4920465 10 11 -1 0.0121518 -1 0.157808 -1 -0.0474793 158 1.859375 14 15 -1 -0.0398807 -1 0.112198 202 0.05147058 17 20 282 31 18 19 -1 -0.00345227 -1 0.019719 13 -0.8461536 21 24 36 0.2577319 22 23 -1 -0.082672 -1 -1.73048e-06 184 -0.566219 25 26 -1 -0.053133 98 0.9499924 27 28 -1 0.00205233"
   "-1 -0.0638024 29 239 -1.233496 1 2 -1 -0.044505 207 -2.904851 3 4 -1 -0.0535083 150 1.225806 5 28 222 -0.2142857 6 23 195 0.6527776 7 20 195 -0.01041666 8 15 100 -0.6047606 9 10 -1 0.0296767 282 92 11 12 -1 -0.0268166 221 -0.8461538 13 14 -1 0.0659787 -1 -0.00309881 115 -0.4450437 16 19 200 0.4999996 17 18 -1 0.0163655 -1 -0.0373213 -1 0.040453 150 -0.1923076 21 22 -1 0.0116151 -1 -0.055435 102 -0.3352452 24 25 -1 -0.0214599 114 -0.3695652 26 27 -1 -0.011559 -1 0.00825951 -1 -0.0335876 29 195 -5.425923 1 2 -1 0.084898 200 93750 3 28 199 -4.349998 4 5 -1 -0.0794088 76 -8.342667 6 7 -1 0.0811311 40 0.3749999 8 13 139 0.5453102 9 10 -1 0.00287582 187 -0.2568208 11 12 -1 0.213856 -1 0.0244636 216 -0.05534409 14 19 222 -4.861109 15 16 -1 0.220825 126 1.562499 17 18 -1 -0.0343864 -1 0.0846545 5 0.123719 20 25 189 -0.4038305 21 22 -1 -0.01598 16 0.5555553 23 24 -1 0.0112074 -1 0.0607577 177 0.6249994"
   "26 27 -1 -0.0368866 -1 0.022446 -1 -0.0579188 29 267 0.6499995 1 26 273 -0.7083332 2 9 130 0.150838 3 8 81 0.1212121 4 5 -1 -0.0104577 181 0.4999998 6 7 -1 0.0416061 -1 -0.0169311 -1 0.0557986 145 -0.1053844 10 15 222 0.3749998 11 12 -1 -0.00132386 188 0.2798125 13 14 -1 0.0553894 -1 -0.0365103 224 0.9074071 16 25 32 0.4509155 17 18 -1 -0.0389999 199 0.0824468 19 20 -1 -0.0242579 200 -0.01515151 21 22 -1 -0.0311844 194 0.864696 23 24 -1 0.0127047 -1 0.224224 -1 0.0286964 260 -0.9499569 27 28 -1 0.114072 -1 0.0109905 29 41 0 1 8 248 -0.02678571 2 3 -1 0.00204483 282 198 4 5 -1 -0.0532495 201 -0.006578946 6 7 -1 -0.0372601 -1 0.101416 187 -0.7068829 9 14 229 -1.423728 10 11 -1 -0.13185 210 0.2386864 12 13 -1 0.0287094 -1 -0.0249167 227 -0.6249975 15 16 -1 0.0200908 197 -2.080882 17 18 -1 0.106849 100 -0.6635434 19 20 -1 -0.032757 110 0.2499995 21 26 204 2.749996 22 25 32 0.6031036 23 24 -1"
   "0.00528881 -1 -0.0210782 -1 -0.187077 98 0.2386354 27 28 -1 0.0203836 -1 -0.00898247 29 8 -2.310031 1 2 -1 -0.0722006 193 0.1303483 3 28 10 -5.284679 4 7 29 0.2829482 5 6 -1 -0.0487884 -1 0.093132 156 -0.173913 8 23 99 0.1796637 9 10 -1 -0.0148933 148 0.1273271 11 14 192 -1.323568 12 13 -1 0.0751803 -1 -0.0469867 24 0.7366881 15 20 109 -0.09090906 16 19 105 -0.9305553 17 18 -1 -0.0433716 -1 0.038525 -1 -0.034041 203 -0.4062498 21 22 -1 -0.0624572 -1 0.090619 233 -0.3267896 24 25 -1 -0.0164349 15 0.06976744 26 27 -1 0.0305061 -1 0.00532099 -1 -0.00686908 29 134 0.7560973 1 26 34 0.7835051 2 21 226 0.932692 3 20 174 0.9551281 4 19 227 0.1507936 5 14 85 -0.08333331 6 13 139 0.6914594 7 12 83 -0.3333332 8 9 -1 0.0220406 132 -0.1363636 10 11 -1 0.00720029 -1 -0.0503122 -1 0.0942651 -1 -0.0201802 131 1.249995 15 18 149 -0.9999996 16 17 -1 -0.0142282 -1 0.0165925 -1 0.0475229 -1 -0.0451237 -1 -0.045198"
   "24 0.0007453508 22 23 -1 0.0821225 173 -1.562499 24 25 -1 0.110765 -1 -0.0248448 280 0.8540773 27 28 -1 0.0193492 -1 -0.0662514 29 243 1.277777 1 28 247 1.999998 2 27 23 0.200185 3 16 236 0.6529381 4 15 58 0.1999998 5 12 221 0.3680555 6 7 -1 -0.0149775 174 0.3648648 8 11 157 -1.306451 9 10 -1 0.105123 -1 0.0198411 -1 -0.0406103 123 -0.6103145 13 14 -1 -0.0189239 -1 0.0713266 -1 0.0599989 32 0.4294311 17 18 -1 -0.0295196 7 -7.946742 19 20 -1 -0.0965831 146 0.1692024 21 24 110 -0.8571424 22 23 -1 -0.0298313 -1 0.00794854 28 1.021491 25 26 -1 -0.0163299 -1 0.0156976 -1 0.0569531 -1 0.0292683 29 281 113 1 18 154 -0.2812499 2 5 182 -0.9041094 3 4 -1 0.0343737 -1 -0.035674 18 -0.999996 6 7 -1 0.042062 5 -0.2263347 8 15 219 -0.3749993 9 12 282 196 10 11 -1 -0.00877154 -1 0.0530422 141 0.1443131 13 14 -1 -0.0157527 -1 -0.0614705 176 -3.692307 16 17 -1 -0.100909 -1 0.00624137 195 0.004504504 19 26"
   "5 -1.235228 20 21 -1 -0.0597527 10 0.04410764 22 25 189 0.2513479 23 24 -1 -0.0092197 -1 0.0598789 -1 0.0388683 206 -1.468749 27 28 -1 -0.105162 -1 -0.00365626 29 175 1.405405 1 28 181 1.299999 2 23 115 0.3852251 3 22 165 -0.20082 4 21 282 103 5 10 280 0.1208791 6 7 -1 0.00298392 195 -1.553571 8 9 -1 0.0900538 -1 -0.0294041 203 0.3999998 11 20 197 0.899999 12 17 40 0.3333332 13 16 172 0.02272726 14 15 -1 0.0133843 -1 0.0706861 -1 -0.00195687 24 0.7267664 18 19 -1 0.0219087 -1 0.191776 -1 -0.0224695 -1 0.00749099 -1 -0.0245119 112 1.531249 24 27 161 0.6845315 25 26 -1 0.0459159 -1 0.207454 -1 -0.0629661 -1 -0.0441306 29 229 -0.4041095 1 8 31 0.03585743 2 7 106 -1.538461 3 6 164 -0.8369906 4 5 -1 0.0540343 -1 -0.390919 -1 0.0720107 -1 -0.0187095 42 0 9 12 65 -0.8148147 10 11 -1 0.0343637 -1 -0.0543172 219 -0.9531245 13 14 -1 0.0743212 282 7 15 16 -1 0.0353647 187 0.1483124 17 26 139 0.505294"
   "18 23 56 -1.00053 19 20 -1 0.0114934 193 -1.634974 21 22 -1 0.0642979 -1 -0.0121308 15 0.6590908 24 25 -1 0.0473371 -1 0.328497 97 0.4636202 27 28 -1 0.0248339 -1 -0.022657 29 247 -1.249999 1 2 -1 0.0362789 107 1.499999 3 28 110 0.2499995 4 5 -1 0.00261115 199 0.04464285 6 7 -1 -0.0210537 166 0.2973858 8 19 165 -0.2759388 9 18 190 0.7847152 10 15 278 11.5 11 12 -1 -0.0112252 78 0.1691589 13 14 -1 0.0297666 -1 0.211495 185 0.610981 16 17 -1 -0.0457215 -1 0.562482 -1 -0.0365632 42 0.08474575 20 27 26 0.9801839 21 22 -1 0.0774204 3 0.2347115 23 26 203 0.04705881 24 25 -1 -0.0140681 -1 0.937967 -1 -0.0350175 -1 0.0226045 -1 0.104844 29 180 562500 1 28 138 0.7069278 2 23 95 0.3394452 3 22 207 0.3587271 4 19 145 0.232837 5 14 231 -0.0270429 6 9 278 3.5 7 8 -1 -0.0114147 -1 0.0272128 38 0.2713178 10 11 -1 -0.0491368 141 -0.8673675 12 13 -1 0.0340461 -1 -0.00469754 138 0.1144455 15 18 131 0.06249998"
   "16 17 -1 -0.0190136 -1 0.0207343 -1 -0.0476588 133 0.249999 20 21 -1 -0.0423412 -1 0.00097565 -1 0.0198707 282 58 24 25 -1 -0.0885849 28 0.6050554 26 27 -1 -0.0680668 -1 0.00816062 -1 0.0877993 29 111 0.8387095 1 18 157 -1.511904 2 3 -1 -0.0300721 149 -2.124999 4 5 -1 0.0820165 200 1.218749 6 9 187 0.3900655 7 8 -1 0.00297769 -1 -0.0210835 201 0.4166661 10 13 194 -1.187961 11 12 -1 0.0904743 -1 -0.0147686 197 0.5078124 14 17 167 1.073524 15 16 -1 0.0806234 -1 0.343908 -1 -0.0373416 193 -0.6984638 19 22 23 0.6352093 20 21 -1 0.0525332 -1 -0.0276834 219 -0.1764705 23 26 209 -0.893678 24 25 -1 -0.0562304 -1 0.0254429 108 62500 27 28 -1 -0.0472777 -1 0.21237 29 38 0.6346153 1 10 260 -0.01962941 2 5 49 -0.6720403 3 4 -1 -0.00371768 -1 0.0329319 63 -0.1999999 6 9 3 -0.6935644 7 8 -1 -0.000345033 -1 0.0223712 -1 -0.012122 223 -0.8749965 11 14 172 -1.620689 12 13 -1 0.302226 -1 -0.0443625 26 0.8304324"
   "15 28 222 -0.4158654 16 17 -1 -0.061179 214 -0.5565617 18 19 -1 0.204336 148 0.1512734 20 27 51 0.2061344 21 26 98 -0.3711316 22 23 -1 -0.0398373 207 0.4846655 24 25 -1 0.118155 -1 -0.0645223 -1 -0.0531701 -1 -0.0383427 -1 -0.0602339 29 203 16.37493 1 26 104 1.399996 2 19 127 1.490196 3 18 18 0.3571428 4 15 116 0.4261546 5 14 41 0.8214285 6 7 -1 -0.00114804 33 0.9702693 8 13 30 0.7099483 9 10 -1 0.035781 84 0.05095541 11 12 -1 0.374135 -1 0.0120813 -1 -0.0376289 -1 0.0309098 107 0.07361962 16 17 -1 -0.0242396 -1 0.0110544 -1 0.0367363 110 -0.8749996 20 21 -1 -0.0937352 217 -0.7836731 22 23 -1 -0.0321923 93 0.2952662 24 25 -1 0.0452066 -1 0.151483 193 -0.5023862 27 28 -1 0.121137 -1 -0.0928814 29 60 -0.6666658 1 2 -1 -0.0140417 97 -0.7481643 3 4 -1 -0.0225284 98 -1.486833 5 6 -1 0.124241 87 -0.999996 7 8 -1 0.0434469 196 -0.6249997 9 12 25 0.1387248 10 11 -1 -0.071411 -1 0.0384736 41 0 13"
   "14 -1 -0.019503 201 -0.2321427 15 22 25 0.3273253 16 17 -1 -0.0282188 184 0.620617 18 19 -1 0.0358324 31 0.4732661 20 21 -1 -0.223537 -1 0.0334362 193 -0.6874389 23 24 -1 -0.0595802 281 221 25 26 -1 0.00139014 225 0.2105262 27 28 -1 -0.227787 -1 -0.00711016 29 204 -0.08333322 1 14 202 -0.1666666 2 7 94 0.4208719 3 6 225 -0.5833326 4 5 -1 0.0258764 -1 -0.0178966 -1 0.0228489 141 0.3345634 8 13 184 -1.795816 9 10 -1 0.142558 46 -1.186431 11 12 -1 -0.0339885 -1 0.0238298 -1 -0.0274058 185 0.07618243 15 22 214 -1.01728 16 17 -1 -0.0553532 41 0.04545454 18 19 -1 0.0182467 214 -0.9878746 20 21 -1 0.108876 -1 -0.0211045 200 1.166665 23 24 -1 -5.15658e-06 36 0.25 25 26 -1 -0.0429209 187 -0.4644412 27 28 -1 0.297276 -1 0.057116 29 280 0.8461538 1 20 281 97 2 9 85 -0.1142857 3 4 -1 0.0175389 30 0 5 6 -1 -0.0423803 214 0.9052851 7 8 -1 0.00446582 -1 -0.0763578 163 -0.8156136 10 15 164 -0.8997292 11"
   "14 1 -1.087347 12 13 -1 0.0184207 -1 -0.0258214 -1 0.0469252 111 -0.4999999 16 17 -1 0.00336772 188 0.7085414 18 19 -1 -0.0332696 -1 0.060981 180 -0.6911763 21 26 186 0.2728055 22 25 194 0.2117968 23 24 -1 0.0156693 -1 -0.0738339 -1 0.0616163 140 -0.8878702 27 28 -1 0.0779336 -1 -0.0553382 29 268 -0.06944443 1 18 282 93 2 13 33 1.002087 3 6 124 0.6936712 4 5 -1 0.0101504 -1 0.0616956 166 -0.453251 7 12 42 0.46 8 9 -1 -0.0452729 174 -0.4915253 10 11 -1 0.0560298 -1 0.405129 -1 0.026971 273 -1.619565 14 15 -1 0.0974393 156 -2.249996 16 17 -1 0.177219 -1 -0.0214644 30 0.06591861 19 22 165 0.9766004 20 21 -1 0.00622652 -1 -0.0282927 273 -0.7083332 23 24 -1 0.0156911 24 0.04237441 25 26 -1 0.0895046 32 0.7197565 27 28 -1 -0.0321303 -1 -0.00680302 29 230 0.06460875 1 16 277 4.5 2 7 238 -0.308092 3 6 274 -1.666664 4 5 -1 0.127131 -1 -0.0274062 -1 0.00324416 231 -0.5073662 8 11 155 -0.4374998 9 10"
   "-1 -0.044078 -1 0.0460961 24 0.4981952 12 13 -1 -0.0185642 48 -0.2396636 14 15 -1 -0.0279381 -1 0.0179963 245 0.6546391 17 24 253 0.136506 18 23 270 -0.6999994 19 20 -1 -0.0422149 38 0.3090909 21 22 -1 -0.0398197 -1 0.000685185 -1 -0.0499863 254 -0.1277917 25 26 -1 -0.0449949 166 -0.734366 27 28 -1 -0.0583709 -1 0.0415343 29 193 -0.03439538 1 12 4 0.265764 2 3 -1 0.000969812 53 -0.0559997 4 5 -1 0.00155761 154 0.3749993 6 11 179 0.2142856 7 10 40 0.4999999 8 9 -1 0.0607087 -1 0.168898 -1 0.0156948 -1 -0.00442823 185 0.1246881 13 18 282 54 14 15 -1 -0.0513387 129 -0.0338983 16 17 -1 0.00982371 -1 -0.0270842 120 -0.8650933 19 20 -1 -0.0357963 176 -2.974999 21 26 190 0.5971192 22 23 -1 0.0376413 171 -1.460634 24 25 -1 0.334599 -1 0.00643187 122 0.7115219 27 28 -1 0.0010044 -1 0.066816 29 65 2.15873 1 28 226 0.9749992 2 27 38 0.3333333 3 10 230 0.1055277 4 9 261 -0.6775531 5 6 -1 -0.038265 153"
   "0.8392857 7 8 -1 0.00024135 -1 0.0629294 -1 0.0164655 223 -0.9999991 11 14 172 -1.620689 12 13 -1 0.26671 -1 -0.00621159 217 0.008748196 15 20 63 0.6666658 16 17 -1 -0.0410703 157 -0.5624999 18 19 -1 0.124925 -1 -0.00707889 227 -0.1499999 21 22 -1 -0.0343199 280 0.7209302 23 26 216 -0.5563622 24 25 -1 0.285993 -1 0.032549 -1 -0.0527249 -1 -0.0303536 -1 0.0312566 29 179 -0.7777774 1 10 111 -0.9999993 2 9 57 -1.235294 3 4 -1 -0.0188956 129 -0.4999998 5 6 -1 -0.042267 74 -0.6512259 7 8 -1 -0.0571297 -1 0.209179 -1 -0.0392346 100 -1.1188 11 28 200 -0.6527775 12 13 -1 -0.0439317 121 -1.491515 14 15 -1 -0.0513412 221 0.15 16 17 -1 0.00841554 135 -0.6041665 18 19 -1 0.193079 169 0.01279699 20 27 282 136 21 26 282 8 22 23 -1 -0.0131555 63 -0.8888885 24 25 -1 0.0214101 -1 0.165283 -1 -0.0363398 -1 0.0119613 -1 0.000233714 29 200 218750 1 26 281 115 2 19 195 0.6363634 3 16 143 -0.5478808 4 7 176 -1.257812"
   "5 6 -1 0.0377151 -1 -0.0377495 119 -1.400393 8 9 -1 0.0520266 18 -0.999996 10 11 -1 0.0436712 41 0.2631576 12 13 -1 -0.0235745 218 -0.1057692 14 15 -1 -0.014452 -1 0.00885012 204 2.749996 17 18 -1 0.0233794 -1 -0.214114 195 0.02083332 20 23 115 -0.8552322 21 22 -1 -0.0289558 -1 0.0252082 206 -1.468749 24 25 -1 -0.100677 -1 -0.00381753 125 0.4311569 27 28 -1 0.0171526 -1 0.178342 29 221 1.062499 1 28 227 1.012195 2 13 70 -1.592347 3 4 -1 -0.0276667 179 -0.9374991 5 12 109 0.1666666 6 11 185 0.3200245 7 8 -1 0.0666412 205 0.6826921 9 10 -1 -0.110026 -1 0.110632 -1 -0.0285067 -1 -0.000532316 218 0.3392855 14 27 222 -0.3185483 15 22 207 -0.1276978 16 19 179 -0.08333322 17 18 -1 0.0942609 -1 -0.018001 203 -0.3636362 20 21 -1 0.0196476 -1 0.215001 200 -0.02499998 23 26 101 0.1916553 24 25 -1 -0.0180442 -1 0.219537 -1 -0.0419875 -1 -0.0195788 -1 -0.0303112 29 195 -5.425923 1 2 -1 0.0784688 200 93750"
   "3 28 153 -4.694442 4 5 -1 -0.0575858 140 -6.682759 6 7 -1 0.126365 151 -0.9999998 8 13 181 -0.7916661 9 10 -1 -0.0881157 13 -1.142857 11 12 -1 0.0156956 -1 -0.0372709 105 -0.2266666 14 15 -1 0.0123336 109 -0.5999998 16 19 131 -0.6730767 17 18 -1 -0.0377504 -1 0.0316035 156 -0.499998 20 23 53 -0.7570381 21 22 -1 -0.0731754 -1 -0.00289845 277 6.5 24 25 -1 0.00154198 237 -0.5032773 26 27 -1 -0.0801054 -1 -0.00813979 -1 -0.0524504 29 238 -2.038045 1 2 -1 0.0736714 44 1.060606 3 22 26 0.2350525 4 5 -1 0.00443192 270 0.7999994 6 19 177 0.8333328 7 18 210 0.1609972 8 15 246 -62500 9 12 34 0.5749999 10 11 -1 0.343957 -1 -0.026162 270 -0.2499999 13 14 -1 0.00395273 -1 -0.023755 227 2.791663 16 17 -1 0.00713064 -1 0.312957 -1 -0.0356763 220 0.5187499 20 21 -1 0.00513477 -1 0.0817301 205 -0.5347221 23 24 -1 0.0747317 38 0.125 25 26 -1 -0.0123779 273 -0.8749991 27 28 -1 0.0836328 -1 0.0153346 29 97 -1.729551"
   "1 2 -1 -0.0640514 197 -2.080882 3 6 118 -0.7783378 4 5 -1 -0.0658978 -1 0.085807 194 -4.420315 7 8 -1 -0.0400531 114 1.781249 9 28 98 0.7264113 10 27 164 0.9765914 11 26 13 -1.037037 12 15 181 -1.722221 13 14 -1 0.156639 -1 -0.0337441 130 0.4375 16 25 186 0.1801341 17 20 10 0.3862917 18 19 -1 0.000676329 -1 0.02214 119 0.4868944 21 24 51 -0.3723505 22 23 -1 -0.0296083 -1 0.00915812 -1 -0.061747 -1 0.0214745 -1 -0.106584 -1 -0.0222537 -1 0.0767657 29 246 -0.07499997 1 2 -1 -0.00582989 39 0.1249999 3 28 137 0.07499997 4 21 219 -2.142856 5 6 -1 -0.190588 48 0.8325216 7 18 34 0.7999999 8 11 202 -0.597222 9 10 -1 0.0287725 -1 -0.0178574 204 -1.299999 12 15 37 0.3749999 13 14 -1 0.0141991 -1 -0.222399 11 0.3749999 16 17 -1 0.0646065 -1 0.0100141 8 -0.5324201 19 20 -1 0.00743704 -1 -0.109157 122 -0.3624806 22 23 -1 0.00427462 92 0.1526774 24 27 160 -0.1794871 25 26 -1 0.0221655 -1 0.0976196 -1 -0.0217458"
   "-1 -0.00128699 29 42 1.27907 1 28 38 0.2833333 2 7 125 0.6716973 3 4 -1 0.000133507 35 0.03584229 5 6 -1 -0.0388562 -1 0.0495219 199 0.2297297 8 25 223 -0.9999991 9 10 -1 0.136314 120 -0.006832803 11 24 39 0.03875968 12 13 -1 -0.0560877 219 -0.1249998 14 15 -1 -0.0399875 133 0.5499996 16 19 34 0.5121951 17 18 -1 0.0520213 -1 -0.0253859 161 0.2377319 20 23 130 0.2159091 21 22 -1 0.102056 -1 0.337991 -1 -0.0558745 -1 -0.046825 34 0.7741934 26 27 -1 0.0415281 -1 -0.0265888 -1 0.0321882 29 247 0.3124999 1 26 26 0.2570495 2 5 280 0.5841584 3 4 -1 -0.00245214 -1 0.0150851 113 0.06666663 6 13 191 1.012175 7 8 -1 -0.0251846 222 0.05833333 9 10 -1 -0.0191683 153 0.7499998 11 12 -1 0.270445 -1 -0.0101739 141 0.9748628 14 23 189 0.4068682 15 22 35 0.1433692 16 19 217 0.1479626 17 18 -1 0.108965 -1 -0.0472338 58 0 20 21 -1 -0.0161092 -1 0.0172865 -1 -0.0262801 159 0.9791663 24 25 -1 -0.0140095 -1 0.360329"
   "255 -0.9942725 27 28 -1 0.104249 -1 0.00988561 29 87 0.2222222 1 20 208 0.6090895 2 19 282 7 3 4 -1 -0.0534875 135 -0.4999997 5 8 76 0.1571476 6 7 -1 0.0336422 -1 -0.00403681 205 1.833331 9 18 268 0.2968749 10 17 24 0.3767994 11 14 106 0.1874998 12 13 -1 -0.0567517 -1 -0.00476787 70 -0.3521545 15 16 -1 0.0159825 -1 -0.0132283 -1 0.0142893 -1 0.134901 -1 -0.0466233 82 -0.7272725 21 24 281 91 22 23 -1 -0.0589663 -1 0.00900877 7 -0.4519286 25 28 245 -0.4814814 26 27 -1 0.0144416 -1 -0.0420856 -1 0.0171048 29 23 0.9843697 1 28 4 0.1603859 2 5 31 0.9008607 3 4 -1 -0.00117147 -1 -0.0626506 33 0.802777 6 7 -1 -0.0257996 212 -1.011902 8 9 -1 -0.0503272 218 -0.4903845 10 17 99 -0.9652594 11 12 -1 0.11631 281 122 13 14 -1 0.0515912 210 -0.03383701 15 16 -1 -0.0533314 -1 0.146128 280 0.8148148 18 27 49 -1.541438 19 20 -1 -0.044946 179 0.1874999 21 26 208 0.3102009 22 23 -1 0.0206465 203 0.09090906 24"
   "25 -1 0.0348718 -1 0.129381 -1 -0.000723073 -1 -0.0438472 -1 -0.0263422 29 40 0.2777777 1 2 -1 0.00459501 24 0.1894119 3 8 168 -1.00189 4 5 -1 0.0370009 208 -0.2567496 6 7 -1 -0.0728261 -1 -0.0176868 30 0.02698445 9 10 -1 -0.0226001 25 0.3919926 11 20 97 0.07818501 12 17 196 -0.1125 13 14 -1 0.00396747 195 -0.0681818 15 16 -1 0.0911138 -1 0.0289447 177 0.6999994 18 19 -1 -0.0226584 -1 0.0355867 223 -0.8214281 21 22 -1 0.135358 217 0.01877689 23 24 -1 -0.0535639 216 0.2343555 25 26 -1 -0.017385 213 0.0860211 27 28 -1 0.259293 -1 -0.0116833 29 175 2.249998 1 26 227 1.708332 2 25 2 -0.4094949 3 16 15 -0.6249999 4 11 131 1.666664 5 10 198 -0.6562493 6 7 -1 0.0231664 127 -0.7154471 8 9 -1 -0.0352328 -1 0.00205124 -1 -0.0360143 184 0.3072296 12 15 228 -0.4642856 13 14 -1 -0.0322938 -1 0.0436093 -1 -0.0147687 96 -0.1114479 17 22 230 0.1887881 18 19 -1 -0.0346125 166 -0.7467973 20 21 -1 0.107706"
   "-1 -0.00758557 108 2.249991 23 24 -1 0.000112784 -1 0.0840693 -1 -0.0465562 166 0.3782552 27 28 -1 -0.0176642 -1 0.120418 29 180 562500 1 28 138 0.7069278 2 25 230 0.07686839 3 14 14 -0.3333333 4 11 211 -1.066046 5 8 282 213 6 7 -1 0.00703251 -1 0.0892572 38 1.085714 9 10 -1 -0.00935174 -1 -0.10023 278 4.5 12 13 -1 -0.0049605 -1 0.0166931 245 0.9047616 15 24 72 -0.001043794 16 17 -1 -0.0286883 117 -0.3188279 18 19 -1 -0.0511225 107 -0.005555555 20 23 23 0.7437973 21 22 -1 -0.033006 -1 0.0376106 -1 -0.0124737 -1 0.0332869 282 60 26 27 -1 -0.0793227 -1 -0.0107724 -1 0.0805017 29 27 0.07804405 1 2 -1 -0.0430891 43 0.02645502 3 18 20 -0.3333329 4 9 265 0.1574074 5 8 72 0.2391866 6 7 -1 0.0221656 -1 -0.0545345 -1 -0.0234193 215 0.112195 10 17 164 0.02562151 11 14 38 0.2363636 12 13 -1 0.102123 -1 0.0104659 201 -0.1249998 15 16 -1 -0.0152024 -1 0.142048 -1 -0.0280717 26 0.01086693 19 20 -1 -0.035178"
   "25 0.001096235 21 26 279 12.5 22 23 -1 0.107887 110 -0.8749996 24 25 -1 0.244129 -1 -0.0169791 111 0.863636 27 28 -1 0.00064518 -1 -0.0226248 29 136 -1.338983 1 4 132 -0.7142853 2 3 -1 9.56769e-05 -1 -0.0591498 5 -1.668421 5 6 -1 0.039095 42 0 7 10 65 -0.8333331 8 9 -1 0.0339487 -1 -0.0510077 229 -2.454545 11 12 -1 0.126822 38 0.2264151 13 16 125 0.6716973 14 15 -1 0.00296853 -1 0.043734 39 0.06430867 17 18 -1 -0.0404988 26 0.35824 19 22 131 0.1249998 20 21 -1 -0.000485544 -1 -0.0596466 25 0.4636729 23 28 74 0.05706206 24 27 197 -1.339285 25 26 -1 -0.0845935 -1 0.0569284 -1 -0.00321852 -1 -0.0199661 29 110 -0.8571424 1 8 185 -1.179708 2 5 112 -0.01388889 3 4 -1 0.0545144 -1 -0.0334525 234 -0.2688733 6 7 -1 0.0311692 -1 -0.0450468 149 -1.033333 9 16 58 -1.222222 10 11 -1 -0.0442066 33 0.999999 12 15 83 -0.235294 13 14 -1 -0.0124079 -1 0.0340194 -1 0.0637911 171 -0.9271842 17 18 -1 -0.0163981"
   "33 0.9523139 19 22 58 0 20 21 -1 -0.00206261 -1 0.0188343 154 -0.4090908 23 28 110 -0.4999998 24 27 99 0.958354 25 26 -1 0.051144 -1 0.332976 -1 -0.00258368 -1 -0.014075 29 24 0.01217762 1 2 -1 -0.0538377 226 93750 3 28 250 0.9062495 4 27 281 114 5 16 198 0.9749992 6 15 98 0.6399964 7 14 204 -0.125 8 9 -1 -0.0160908 121 -1.437489 10 11 -1 0.0496975 8 -0.3709533 12 13 -1 -0.0180742 -1 0.0083877 -1 -0.0266628 -1 0.0407263 203 -0.05999999 17 22 205 -1.296296 18 19 -1 -0.0423451 137 -0.4285712 20 21 -1 0.0729985 -1 0.0183209 256 -0.08021603 23 24 -1 -0.0301388 206 -1.359374 25 26 -1 -0.0776151 -1 0.0125645 -1 -0.0199052 -1 0.0675095 29 247 -0.04761904 1 6 36 0.8545454 2 5 209 0.1841088 3 4 -1 -0.0154299 -1 0.00659674 -1 0.0106066 237 0.005571376 7 14 24 0.0007453508 8 9 -1 -0.0458419 130 0.3055555 10 13 150 0.8999993 11 12 -1 0.0197614 -1 -0.0264716 -1 -0.0109185 93 -0.5280705 15 22 136 -0.1547619"
   "16 19 110 0.5499998 17 18 -1 -0.00986577 -1 0.126709 179 -0.5999995 20 21 -1 0.0359217 -1 -0.0538156 243 -0.5089285 23 26 141 0.03828233 24 25 -1 -0.00631825 -1 0.101417 87 0.1999998 27 28 -1 0.00948823 -1 -0.0229787 29 134 0.7560973 1 28 34 0.5882352 2 23 30 0 3 10 107 -0.9230768 4 5 -1 0.0196367 23 0.008776813 6 7 -1 0.056395 227 0.08333328 8 9 -1 -0.0898008 -1 -0.0246463 207 0.3251957 11 20 143 0.601775 12 19 31 0.8255675 13 18 177 -0.01470588 14 15 -1 0.0182322 118 -0.9123331 16 17 -1 -0.0350797 -1 0.00467738 -1 -0.0385139 -1 0.0378046 177 1.374997 21 22 -1 0.0189619 -1 0.108308 24 0.0007453508 24 25 -1 0.0510475 149 -1.274999 26 27 -1 -0.058063 -1 -0.010688 -1 0.0134606 29 241 -1.216666 1 4 178 0.2105261 2 3 -1 -0.00121203 -1 0.0511899 7 -0.9450039 5 8 193 -1.103116 6 7 -1 0.049003 -1 -0.0155174 35 1.12 9 28 24 0.778299 10 13 171 0.1670811 11 12 -1 0.00886194 -1 -0.0115863 107 0.02222222"
   "14 15 -1 -0.0366917 73 0.07618127 16 17 -1 -0.036982 243 0.6874999 18 25 200 -0.7692305 19 20 -1 0.232139 201 0.6249992 21 22 -1 -0.00918824 204 0.2884614 23 24 -1 0.179692 -1 -0.0298431 84 0.3864734 26 27 -1 0.229409 -1 -0.0602904 -1 0.0328143 29 7 1.116324 1 28 197 -2.080882 2 5 111 -0.1041666 3 4 -1 -0.00980566 -1 0.102505 282 194 6 15 41 0 7 14 248 -0.02678571 8 9 -1 -0.00273302 27 0.2127361 10 13 35 0.6666664 11 12 -1 -0.029212 -1 0.251996 -1 -0.0636157 -1 0.00104461 24 0.821184 16 23 40 0.4615383 17 22 203 0.4999995 18 21 206 0.5090908 19 20 -1 -0.0344757 -1 0.0980959 -1 0.0812069 -1 0.0606513 106 -0.03260869 24 27 219 -0.3749993 25 26 -1 0.0187555 -1 -0.179991 -1 0.0256126 -1 -0.0311063 29 43 0.6428571 1 24 45 0.6363635 2 9 219 -1.549999 3 4 -1 -0.0698271 199 -1.118055 5 6 -1 -0.0345549 176 -4.999998 7 8 -1 -0.0854873 -1 0.00196682 148 0.1557482 10 21 189 1.125795 11 18 281 213 12"
   "15 220 -0.7884613 13 14 -1 0.0386443 -1 -0.00865882 100 -0.1678991 16 17 -1 0.00221787 -1 0.205121 105 0.08720929 19 20 -1 -0.0245196 -1 0.216464 177 0.999996 22 23 -1 0.0380492 -1 -0.0423197 70 0.4624954 25 28 171 -0.08065039 26 27 -1 0.00672024 -1 -0.0167176 -1 -0.047793 29 19 0.6666664 1 10 42 0.8095237 2 5 226 0.932692 3 4 -1 0.00272019 -1 -0.0381714 143 -0.8992937 6 7 -1 0.030246 223 -0.9999991 8 9 -1 0.115313 -1 -0.0450879 126 -0.3181817 11 12 -1 -0.0288725 197 0.5374999 13 24 282 41 14 19 157 0.2916666 15 16 -1 0.0811753 281 92 17 18 -1 0.0603835 -1 -0.0215845 148 1.027346 20 23 193 -0.3044322 21 22 -1 -0.0402279 -1 0.0196268 -1 0.185954 48 -1.187999 25 26 -1 0.0549756 133 -0.3749993 27 28 -1 0.0168876 -1 -0.0632122 29 258 0.03925923 1 24 258 -0.8360525 2 5 240 -2.33211 3 4 -1 -0.0640735 -1 0.0287151 282 91 6 23 93 0.3415763 7 18 281 26 8 13 217 0.4806798 9 10 -1 0.0302266 224 0.2321427"
   "11 12 -1 0.0361968 -1 0.458257 94 -0.28239 14 15 -1 0.00236648 101 0.6833342 16 17 -1 -0.0281062 -1 0.0364171 281 61 19 20 -1 -0.00796932 275 -0.9166664 21 22 -1 0.184532 -1 0.0455349 -1 -0.0188392 276 0.5 25 28 110 0.4821427 26 27 -1 0.0274715 -1 -0.0251532 -1 0.000732956 29 200 218750 1 26 79 0.8066242 2 23 10 0.3862917 3 22 202 -1.124999 4 9 223 0.999998 5 8 133 -0.3749999 6 7 -1 -0.118066 -1 0.0182069 -1 -0.30606 3 -4.068119 10 11 -1 0.0610587 94 -0.3536489 12 17 237 0.4888908 13 16 210 0.7664399 14 15 -1 -0.0277845 -1 -0.154555 -1 0.0157307 38 1.017857 18 21 148 0.3455314 19 20 -1 0.00709236 -1 -0.0214876 -1 -0.0296293 -1 0.0109685 197 0.2567567 24 25 -1 -0.0410445 -1 0.0152337 125 0.4311569 27 28 -1 0.0150611 -1 0.163601 29 239 -1.233496 1 2 -1 -0.0401167 199 1.512499 3 26 241 0.2272726 4 25 188 0.09663133 5 24 34 0.9411764 6 21 4 0.2947655 7 14 34 0.6466512 8 11 184 0.02191507 9 10"
   "-1 0.00367117 -1 0.0362842 15 0.06976744 12 13 -1 0.0118281 -1 -0.0244509 109 0.1538461 15 16 -1 0.00724828 282 44 17 20 118 -0.4696593 18 19 -1 -0.0264795 -1 0.165148 -1 0.0288086 150 0.6666661 22 23 -1 0.0331595 -1 -0.0680314 -1 -0.00564854 -1 -0.0064793 218 -0.2027026 27 28 -1 0.0383423 -1 -0.101996 29 280 0.8461538 1 18 281 97 2 9 282 11 3 6 138 -0.7186863 4 5 -1 0.0627641 -1 -0.00606149 85 -0.1142857 7 8 -1 0.0137713 -1 -0.0050752 282 127 10 13 1 -2.430685 11 12 -1 0.0353566 -1 -0.0122774 202 -0.999996 14 15 -1 -0.111932 148 -3.441182 16 17 -1 -0.0388359 -1 0.0706834 174 -0.7124999 19 22 113 -1.846153 20 21 -1 -0.136543 -1 0.0210432 140 -0.8878702 23 24 -1 0.0560165 282 52 25 26 -1 0.208443 29 0.2240475 27 28 -1 0.0511712 -1 -0.056849 29 247 -1.249999 1 4 210 -0.03383701 2 3 -1 0.0732619 -1 0.00101879 236 0.8593707 5 28 268 -0.06944443 6 11 208 -0.9328479 7 8 -1 -0.0404625 267 1.117647"
   "9 10 -1 0.0106575 -1 0.0785686 25 0.155233 12 17 163 0.1327566 13 16 197 -0.5624998 14 15 -1 0.0140702 -1 -0.0114412 -1 0.0150216 40 1.315789 18 25 264 -1.272727 19 20 -1 0.0464254 144 -0.1052625 21 22 -1 -0.0377239 26 0.09589167 23 24 -1 0.0977258 -1 -0.0135001 132 0.04545454 26 27 -1 0.164259 -1 0.00189465 -1 0.0153657 29 138 0.6486447 1 26 95 0.3394452 2 17 210 0.3612511 3 10 37 0.5999998 4 7 102 0.2210415 5 6 -1 -0.0151309 -1 0.0085227 244 0.3050847 8 9 -1 0.0190221 -1 -0.00654493 27 0.4427079 11 14 5 -0.7817159 12 13 -1 -0.0565348 -1 0.0652151 190 -0.4687483 15 16 -1 -0.0895874 -1 -0.0255427 191 0.7652484 18 23 107 -0.5163398 19 20 -1 -0.0978699 144 1.077771 21 22 -1 0.0351196 -1 -0.108476 110 -0.2499995 24 25 -1 -0.141126 -1 -0.00598824 282 60 27 28 -1 -0.0680427 -1 -0.00659496 29 228 0.2272726 1 22 95 0.3812123 2 19 141 0.150343 3 14 204 -0.1249999 4 7 232 0.9728068 5 6 -1 0.0116151"
   "-1 -0.0683566 84 0.4324324 8 13 209 0.1841088 9 10 -1 -0.00832234 184 0.9492184 11 12 -1 0.02026 -1 0.223061 -1 -0.0366861 1 -0.4942095 15 16 -1 -0.013999 281 114 17 18 -1 0.0431466 -1 -0.0137672 185 1.016898 20 21 -1 -0.0330059 -1 0.108036 175 -1.583332 23 24 -1 0.0945015 281 187 25 26 -1 -0.0111149 198 -0.02499998 27 28 -1 -0.00605984 -1 0.101906 29 245 -0.02777777 1 12 277 6.5 2 9 238 -0.3544396 3 6 253 1.061103 4 5 -1 0.0233483 -1 -0.0396344 239 0.4556492 7 8 -1 -0.0122072 -1 0.0213858 237 -0.5032773 10 11 -1 -0.0746334 -1 -0.011983 254 0.282049 13 18 125 -0.9987841 14 15 -1 0.0172881 34 0 16 17 -1 -0.0569291 -1 -0.00323368 2 0.8775239 19 26 179 0.3249999 20 25 281 177 21 24 152 0.3124999 22 23 -1 0.0525971 -1 -0.0134487 -1 -0.0436893 -1 -0.00158376 112 0 27 28 -1 0.0171788 -1 0.401979 29 203 16.37493 1 26 190 0.008064475 2 13 156 1.437499 3 12 224 0.8333329 4 9 145 -0.7117565 5 6 -1"
   "0.00663061 129 0.2999998 7 8 -1 -0.0253563 -1 0.0029749 214 0.3508876 10 11 -1 0.00481898 -1 0.0742536 -1 0.129208 227 1.708332 14 25 173 -0.8499993 15 16 -1 -0.0365706 165 -0.1315031 17 24 28 0.9530629 18 21 220 0.9499998 19 20 -1 0.0118926 -1 0.110301 225 -0.4999995 22 23 -1 0.201382 -1 0.0496326 -1 0.0041595 -1 -0.0669117 193 -0.5023862 27 28 -1 0.110163 -1 -0.0884094 29 134 562500 1 28 261 0.5223204 2 23 205 1.833331 3 22 11 -1.444444 4 7 121 1.947354 5 6 -1 -0.0366248 -1 0.204619 3 -4.068119 8 11 208 0.01635763 9 10 -1 0.00756603 -1 0.1417 121 0.9451448 12 19 191 -3.00964 13 16 123 -0.718592 14 15 -1 -0.0830705 -1 0.0764699 250 0.9062495 17 18 -1 0.00297937 -1 -0.0193031 71 0.7117091 20 21 -1 -0.0214213 -1 -0.149894 -1 0.0957359 233 -0.5511629 24 25 -1 0.0199432 92 0.1526774 26 27 -1 -0.047571 -1 0.0148955 -1 0.0743686 29 221 1.624994 1 28 227 1.012195 2 17 115 0.3786316 3 4 -1 0.000649525"
   "173 1.142857 5 12 173 0.1515151 6 11 148 0.1398644 7 8 -1 -0.0228154 94 0.07706337 9 10 -1 -0.0339862 -1 0.117613 -1 -0.0498439 132 0.1499999 13 16 202 0.2499998 14 15 -1 -0.0106666 -1 0.349716 -1 -0.0468851 214 -2.194359 18 19 -1 0.114374 118 0.7642494 20 27 154 -0.1111111 21 26 280 0.4701493 22 25 279 16.5 23 24 -1 0.166325 -1 0.00447969 -1 -0.00652659 -1 -0.0124291 -1 0.190886 -1 -0.0564797 29 136 -1.338983 1 4 85 0 2 3 -1 -0.0140545 -1 -0.111351 52 -1.941163 5 6 -1 0.0382467 44 0.0696864 7 8 -1 -0.0287117 36 0.2857143 9 12 264 0.2499997 10 11 -1 0.0227887 -1 -0.00256478 19 0.6666664 13 22 42 0.8095237 14 19 24 0 15 16 -1 0.0755618 151 -0.9999998 17 18 -1 -0.0346494 -1 0.000378389 223 -0.8749965 20 21 -1 0.115644 -1 -0.0424747 194 -0.6391587 23 26 131 0.3076922 24 25 -1 -0.0565244 -1 0.026812 102 -0.2050767 27 28 -1 0.0493642 -1 0.00539786 29 243 1.277777 1 28 228 1.529411 2 27 14 2.874999"
   "3 26 34 0.8297872 4 21 177 0.6249988 5 16 113 0.1666666 6 7 -1 0.00139022 126 -3.787877 8 9 -1 -0.0948448 166 -0.3467726 10 11 -1 0.0493809 132 -0.2499999 12 15 129 0.9062498 13 14 -1 -0.0200218 -1 0.0767127 -1 0.0255292 249 1.172413 17 20 26 0.267892 18 19 -1 0.00129922 -1 -0.0322532 -1 0.0747195 264 -0.7499994 22 23 -1 0.00892905 175 0.5312497 24 25 -1 -0.0273092 -1 0.0105259 -1 0.0720217 -1 -0.0621198 -1 0.0257586 29 40 0.03125 1 12 247 -0.0340909 2 9 223 -0.5384609 3 6 23 0.4284789 4 5 -1 -0.0139256 -1 0.108507 232 -0.03453796 7 8 -1 0.0128448 -1 -0.0357005 158 -1.549999 10 11 -1 0.127628 -1 -0.0529883 226 3.374998 13 28 3 -4.068119 14 15 -1 0.0536384 250 0.9062495 16 27 202 -0.215909 17 22 282 226 18 21 8 -1.0497 19 20 -1 -0.0563835 -1 0.0128266 -1 0.0763788 194 -4.420315 23 24 -1 -0.070004 221 1.124999 25 26 -1 -0.00240425 -1 0.0549123 -1 -0.0216562 -1 0.0593544 29 8 -2.310031 1 2 -1"
   "-0.0676719 6 -0.2352923 3 16 213 -1.603165 4 9 207 -1.046991 5 8 55 -2.348041 6 7 -1 0.323387 -1 -0.00513821 -1 -0.0876901 281 18 10 13 282 103 11 12 -1 -0.0253824 -1 0.0384323 26 0.00025822 14 15 -1 -0.0822338 -1 0.00476002 111 -1.394736 17 18 -1 -0.0484358 10 0.4557588 19 28 200 31250 20 21 -1 0.000456529 23 0.877918 22 27 205 0.6179774 23 26 201 0.3333329 24 25 -1 0.0613735 -1 0.242399 -1 -0.0552523 -1 -0.051673 -1 -0.0163379 29 139 0.468045 1 22 16 -1.53846 2 3 -1 0.0725769 245 -0.02941176 4 9 277 6.5 5 6 -1 -0.00362907 237 -0.1058866 7 8 -1 -0.0651694 -1 -0.00227921 277 2.5 10 11 -1 -0.0078451 281 34 12 13 -1 -0.0188173 187 0.4033532 14 21 38 0.09070294 15 20 36 0 16 17 -1 -0.0251372 181 2.187498 18 19 -1 0.0219179 -1 0.12675 -1 -0.0023638 -1 0.0616957 184 0.4329401 23 26 169 0.7051471 24 25 -1 0.0624394 -1 -0.0582822 116 0.04518105 27 28 -1 0.00711435 -1 -0.0607345 29 185 0.7363017"
   "1 28 79 -4.74587 2 3 -1 -0.0225201 259 -1.439646 4 5 -1 0.0359774 282 7 6 9 163 -0.9656395 7 8 -1 0.0879584 -1 -0.0332683 23 0.1670381 10 15 11 0.7999994 11 14 60 -0.9890108 12 13 -1 -0.0227928 -1 0.0404402 -1 -0.0533228 90 -0.2499999 16 19 169 0.8086072 17 18 -1 -0.00667936 -1 -0.0637541 33 0.9433899 20 27 7 -0.9326972 21 22 -1 -0.00955208 129 0.458333 23 26 87 0.3333331 24 25 -1 0.0184524 -1 -0.0192016 -1 0.0384423 -1 -0.0057577 -1 -0.0274731 29 63 -0.4999995 1 6 53 0.07444661 2 5 26 0.4763935 3 4 -1 -0.0180702 -1 0.0259389 -1 -0.0244066 59 -0.736842 7 8 -1 -0.0200168 61 0.02083333 9 26 37 0.3797467 10 13 56 0.270526 11 12 -1 -0.0357427 -1 0.0107893 151 -0.07812498 14 17 141 -0.907994 15 16 -1 0.0371182 -1 -0.0284396 122 0.1597036 18 25 1 -0.5708543 19 20 -1 0.0472642 94 -0.4336135 21 22 -1 -0.0278103 26 0.4763935 23 24 -1 -0.033395 -1 0.0325237 -1 -0.0263115 98 0.9774389 27 28 -1 0.011582"
   "-1 -0.0480937 29 23 0.9827959 1 28 24 0.7366881 2 3 -1 -0.00251739 28 0.4965325 4 15 206 0.122807 5 10 134 0.1029412 6 9 117 0.4230834 7 8 -1 0.0117837 -1 0.128628 -1 -0.0271163 197 0.57971 11 14 93 0.1683209 12 13 -1 0.100172 -1 -0.0219949 -1 0.183715 203 -0.2083333 16 19 205 1.374995 17 18 -1 -0.0288221 -1 0.205254 21 -0.8999998 20 21 -1 0.0919391 140 0.06364163 22 27 15 -0.6595744 23 24 -1 0.0860975 13 -0.6666665 25 26 -1 0.0596374 -1 -0.0297402 -1 0.0195185 -1 -0.0243564 29 65 1.599999 1 24 226 0.7874997 2 19 76 2.030592 3 18 38 0.5192307 4 7 260 -0.01962941 5 6 -1 0.0173344 -1 -0.000815141 281 107 8 17 282 111 9 16 210 -2.27841 10 11 -1 0.232874 1 0.1153262 12 15 217 0.04645056 13 14 -1 -0.00420086 -1 0.0842889 -1 -0.057588 -1 -0.0561712 -1 -0.0624661 -1 0.107585 31 0.1635279 20 21 -1 0.0468202 19 0.9999996 22 23 -1 -0.0533471 -1 0.000183946 188 -0.3536413 25 26 -1 -0.0212199 195 0.2329545"
   "27 28 -1 0.0684895 -1 0.00536947 29 100 -1.1188 1 10 220 -0.6607141 2 3 -1 -0.0338063 138 -0.119438 4 9 137 -0.9999996 5 6 -1 0.119911 200 -0.6527775 7 8 -1 -0.0563151 -1 0.0200461 -1 0.122176 280 0.05309735 11 12 -1 -0.0429274 105 -1.249999 13 18 181 -0.7499998 14 17 191 -0.945194 15 16 -1 0.241917 -1 -0.0212615 -1 -0.0342241 151 -0.6923075 19 28 26 0.2783841 20 25 189 -0.04204281 21 22 -1 0.0168487 34 0.1111111 23 24 -1 -0.0589632 -1 0.104214 15 0.1764706 26 27 -1 -0.0248505 -1 0.060137 -1 -0.00165415 29 207 0.3587271 1 6 136 1.272727 2 5 138 0.7069278 3 4 -1 0.00228017 -1 -0.0290668 -1 0.0523829 133 0.2499995 7 18 140 -0.4681654 8 11 218 0.6964282 9 10 -1 -0.0731722 -1 0.0946089 269 -0.3571427 12 13 -1 0.0243357 203 0.2279411 14 15 -1 -0.00989402 192 -0.2523187 16 17 -1 -0.130153 -1 -0.0326083 25 0.3463154 19 22 245 -0.6999999 20 21 -1 0.0248249 -1 -0.0574427 179 1.19375 23 26 179 0.2499995"
   "24 25 -1 -0.0123831 -1 0.0459192 114 0.3392856 27 28 -1 -0.15394 -1 0.0393288 29 218 2.968749 1 28 170 0.5150514 2 11 227 0.1666666 3 4 -1 0.00275927 190 -0.4798362 5 10 105 0.05524861 6 9 177 0.3599999 7 8 -1 0.00338148 -1 0.0647183 -1 -0.0317731 -1 -0.0177973 203 0.01562499 12 19 25 0.08816359 13 18 214 -0.9526598 14 17 36 0.8421052 15 16 -1 -0.0743988 -1 0.149013 -1 -0.0742057 -1 0.008482 179 0.03448274 20 25 177 -0.6999999 21 22 -1 -0.076366 207 0.3362334 23 24 -1 0.0483682 -1 0.13049 168 -1.563193 26 27 -1 0.14272 -1 -0.00681691 -1 -0.0764614 29 182 0.225 1 24 106 0.3499997 2 15 169 0.7394502 3 12 139 0.6914594 4 11 213 0.2819132 5 10 108 0.8499993 6 9 148 0.3882702 7 8 -1 0.00186785 -1 -0.02663 -1 -0.042027 -1 0.00956077 -1 0.0581553 178 0.08536584 13 14 -1 0.0702368 -1 -0.019534 28 0.932647 16 23 176 0.8305084 17 20 253 0.153861 18 19 -1 0.00152954 -1 -0.0422932 223 0.4374996 21 22"
   "-1 -0.00691921 -1 0.267173 -1 -0.0590639 175 -0.9333331 25 26 -1 -0.0666756 282 24 27 28 -1 0.0345729 -1 0.00399826 29 241 0.2499999 1 20 101 1.688481 2 19 34 0.8297872 3 12 177 1.559524 4 11 103 0.1666664 5 10 197 -0.864583 6 7 -1 0.0198755 281 146 8 9 -1 -0.00405237 -1 -0.037982 -1 0.0140643 -1 -0.0315524 210 0.1406103 13 16 274 -1.229885 14 15 -1 0.0403876 -1 -0.0411126 35 1.083333 17 18 -1 0.00250892 -1 0.103827 -1 0.0795031 79 1.09183 21 28 282 136 22 27 127 0.999998 23 24 -1 0.0092721 202 0.328125 25 26 -1 0.0668749 -1 -0.00740167 -1 -0.0218927 -1 -0.0653906 29 197 -1.857142 1 4 70 -0.2823918 2 3 -1 -0.00943394 -1 0.0797496 189 -0.8485161 5 22 26 0.6122659 6 7 -1 -0.0536624 281 241 8 21 145 -0.9375796 9 10 -1 0.0572723 211 -0.07974816 11 18 134 0.4545453 12 17 110 -0.499999 13 14 -1 -0.0409105 185 -0.1155745 15 16 -1 0.0480172 -1 -0.0858595 -1 -0.070549 196 0.2499999 19 20 -1 -0.0531692"
   "-1 0.0751933 -1 0.104035 193 -1.016779 23 24 -1 0.0710238 281 253 25 28 196 -1.333333 26 27 -1 0.0809732 -1 5.75036e-05 -1 -0.102393 29 197 -0.3181817 1 26 148 0.4089405 2 17 135 -0.3333332 3 6 118 0.0008268918 4 5 -1 -0.0261216 -1 -0.0771226 144 -0.2192966 7 16 128 -0.05769229 8 15 18 0.4137929 9 10 -1 0.015802 96 -0.2011109 11 12 -1 0.0462462 147 -0.05965082 13 14 -1 0.305928 -1 0.0311802 -1 -0.0246475 -1 -0.0192989 196 -0.1785714 18 25 1 0.1930769 19 24 56 0.6060538 20 23 281 180 21 22 -1 0.0151775 -1 -0.0511539 -1 0.0399021 -1 0.119832 -1 -0.0307729 194 -4.420315 27 28 -1 0.0633621 -1 0.00161415 29 200 93750 1 26 207 -3.712738 2 3 -1 0.0981939 153 -4.694442 4 7 98 -1.244644 5 6 -1 0.0287744 -1 -0.0895931 140 -6.682759 8 9 -1 0.117105 136 -1.338983 10 19 154 0.7499999 11 16 266 -0.6458332 12 13 -1 -0.0651551 148 -17.82119 14 15 -1 -0.158396 -1 0.0144031 241 -0.6999994 17 18 -1 0.0172639"
   "-1 -0.151267 5 -1.668421 20 21 -1 0.0374838 34 0 22 25 10 -1.230522 23 24 -1 0.0097909 -1 -0.0576489 -1 0.0023843 138 -1.09387 27 28 -1 0.044273 -1 -0.0720802 29 258 0.03925923 1 8 270 -0.7222219 2 7 99 0.2511643 3 4 -1 0.00624801 10 0.5204695 5 6 -1 0.0413259 -1 0.230025 -1 -0.00738645 279 28.5 9 26 53 -0.05139519 10 15 276 2.5 11 12 -1 0.0186877 247 -0.9999997 13 14 -1 0.0786192 -1 -0.00780663 260 -0.007325573 16 25 198 0.564516 17 18 -1 0.00300671 261 0.164714 19 20 -1 -0.0326692 126 -0.04411764 21 22 -1 0.0194868 160 0.5111109 23 24 -1 0.316401 -1 0.0332742 -1 -0.0287031 149 0.3749998 27 28 -1 -0.0309429 -1 0.0275609 29 114 2.249999 1 28 24 0.03178299 2 3 -1 -0.0416064 18 -0.999996 4 15 186 0.6484806 5 14 26 0.1121989 6 11 30 0.06139127 7 8 -1 0.0330652 150 -0.1346153 9 10 -1 -0.0203258 -1 0.332379 224 -0.2499998 12 13 -1 -0.0198932 -1 0.053965 -1 -0.067792 200 218750 16 27 281 115 17"
   "18 -1 -0.00557054 18 -0.5192307 19 20 -1 -0.0270648 18 -0.4999995 21 24 32 0.4676513 22 23 -1 0.137828 -1 0.0032573 29 0.4557633 25 26 -1 -0.0300879 -1 0.0124527 -1 0.0728185 -1 0.0831377 29 27 0.07804405 1 6 60 -0.9999998 2 5 3 -0.6208742 3 4 -1 -0.0108655 -1 0.184794 -1 -0.0632085 158 1.107142 7 16 181 1.299999 8 9 -1 0.000319269 175 1.230769 10 15 1 0.1062129 11 12 -1 0.0300667 178 0.9545451 13 14 -1 0.0798216 -1 0.364866 -1 -0.0426919 226 -2.458333 17 18 -1 0.124791 209 0.1540825 19 20 -1 -0.0546067 24 0.6691222 21 22 -1 -0.0355725 72 0.1447695 23 28 179 -2.775558e-17 24 25 -1 0.0531229 220 4.163336e-17 26 27 -1 -0.034048 -1 0.41711 -1 -0.0100956 29 195 -5.425923 1 2 -1 0.068004 203 -3.068181 3 8 1 -0.9924304 4 7 188 -0.8166231 5 6 -1 -0.0675573 -1 0.0600697 -1 -0.0964387 229 -0.4041095 9 24 146 -1.169589 10 11 -1 -0.069417 131 0.499999 12 17 149 -0.9166663 13 16 175 -0.3050847 14 15"
   "-1 0.0758671 -1 -0.0644121 -1 -0.00593449 207 -0.4546215 18 21 229 -1.522727 19 20 -1 0.05619 -1 -0.105908 97 -0.3229026 22 23 -1 -0.0898647 -1 0.0489769 280 0.6076389 25 28 281 60 26 27 -1 0.0322697 -1 0.0027475 -1 -0.00863454 29 271 1.499997 1 28 7 -7.946742 2 3 -1 -0.0595976 99 -6.794705 4 5 -1 0.114289 110 -0.8571424 6 13 119 0.6072325 7 12 156 -1.599995 8 11 280 0.5053763 9 10 -1 -0.0256154 -1 0.137443 -1 -0.0190624 -1 -0.0713996 143 -0.378895 14 27 211 0.540022 15 22 132 -0.6666665 16 19 222 0.3999998 17 18 -1 0.0241056 -1 0.143557 96 -0.2843744 20 21 -1 -0.0142534 -1 0.0155035 24 0.4797354 23 26 63 -0.892857 24 25 -1 -0.0422125 -1 0.0604371 -1 -0.0262263 -1 -0.00338718 -1 0.0346974 29 181 -0.4583332 1 6 191 0.3163003 2 3 -1 0.0165764 49 0.1065601 4 5 -1 -0.0456947 -1 0.0249247 31 0.1958253 7 8 -1 -0.0299626 27 0.2127361 9 12 208 -0.1171558 10 11 -1 -0.00851664 -1 0.103394 105 -0.07954544"
   "13 20 109 0.3157894 14 19 202 -1.199999 15 18 223 0.5694442 16 17 -1 -0.0312483 -1 -0.265399 -1 0.000208952 -1 -0.034142 110 0.2222221 21 24 219 0.1029412 22 23 -1 -0.0183068 -1 0.00576609 187 0.1089692 25 26 -1 0.0238632 78 -1.387495 27 28 -1 0.132994 -1 -0.00660061 29 206 2.01282 1 24 198 0.9749992 2 23 202 0.678571 3 12 187 0.3900655 4 5 -1 0.000421035 134 0.3333332 6 11 39 0.04761904 7 10 176 0.5208332 8 9 -1 -0.0206922 -1 0.136645 -1 -0.0641726 -1 0.0113458 207 0.2297114 13 20 182 0.2916666 14 19 30 0.1103484 15 16 -1 -0.0282304 152 0.2499999 17 18 -1 0.00710426 -1 0.0612037 -1 -0.0399655 221 0.4722221 21 22 -1 0.0804942 -1 -0.0206398 -1 -0.033914 114 1.781249 25 28 225 0.1666664 26 27 -1 0.0101137 -1 0.242855 -1 -0.0547319 29 88 1.299999 1 22 226 0.8402776 2 19 186 -1.351333 3 6 185 -3.416751 4 5 -1 -0.0651045 -1 0.0355085 200 62500 7 16 179 -2.33333 8 9 -1 0.110739 179 -0.9038459 10"
   "11 -1 -0.0328088 163 0.04788505 12 13 -1 0.00507569 214 0.02992546 14 15 -1 -0.0343993 -1 -0.00180945 57 -0.7272726 17 18 -1 -0.00584207 -1 -0.106547 179 -0.4166661 20 21 -1 0.00999146 -1 -0.0444449 185 -0.005665 23 24 -1 -0.00199476 180 -0.3076922 25 26 -1 -0.0743962 152 -0.7499993 27 28 -1 -0.0740779 -1 0.0529126 29 221 -1.714285 1 2 -1 -0.067585 238 -2.038045 3 6 123 -0.6002714 4 5 -1 -0.0241644 -1 0.12148 7 -7.946742 7 12 11 -2.437499 8 11 40 0 9 10 -1 -0.049187 -1 0.151797 -1 -0.087545 1 -4.096528 13 14 -1 0.0787085 40 1.272727 15 24 236 1.015616 16 23 175 0.4411763 17 22 213 0.3571408 18 19 -1 -0.000352192 102 0.0841831 20 21 -1 -0.00775764 -1 -0.0403989 -1 0.00716846 -1 0.0171826 109 0.2647058 25 28 30 0.2613138 26 27 -1 0.0135522 -1 0.122851 -1 0.00170787 29 233 0.6698443 1 22 230 0.0669812 2 19 277 4.5 3 8 238 -0.308092 4 7 274 -1.666664 5 6 -1 0.110422 -1 -0.023374 -1 0.0023331"
   "233 -0.1376675 9 14 143 -0.4820205 10 11 -1 -0.0192666 76 -0.8888009 12 13 -1 0.0614162 -1 0.0189386 26 0.9557858 15 18 228 -0.7569443 16 17 -1 0.0256386 -1 -0.0185641 -1 0.0233837 253 0.3586595 20 21 -1 -0.00908972 -1 -0.0632792 254 -0.1277917 23 24 -1 -0.0544154 261 0.2393939 25 26 -1 0.0448222 164 -0.1197411 27 28 -1 -0.133055 -1 0.00957017 29 199 1.512499 1 28 212 -0.9892994 2 11 229 -0.7569443 3 6 194 -1.271372 4 5 -1 -0.0898113 -1 0.0121518 37 0.4615384 7 8 -1 -0.0556487 135 -0.5624998 9 10 -1 0.0673111 -1 -0.0238454 207 -1.112763 12 17 173 -0.07228914 13 16 70 0.497955 14 15 -1 0.041805 -1 0.282855 -1 -0.0131851 213 -1.784991 18 21 151 0.7999997 19 20 -1 -0.115026 -1 0.119317 24 5.525552e-05 22 23 -1 -0.0883132 202 0.7291662 24 25 -1 -0.000568848 145 0.7217784 26 27 -1 0.0117824 -1 0.0943555 -1 -0.0556559 29 63 -0.2926828 1 16 49 -0.6990203 2 11 48 -1.475249 3 6 164 0.09897347 4 5"
   "-1 0.00471286 -1 0.054717 164 -0.3420139 7 10 254 0.1565516 8 9 -1 -0.00194113 -1 0.0557351 -1 -0.0212675 99 -0.5516109 12 15 65 0.7241378 13 14 -1 0.040854 -1 0.125251 -1 0.00840173 83 -0.3529411 17 20 274 -1.0875 18 19 -1 -0.0469725 -1 0.0130203 156 0.1666664 21 24 212 0.6323915 22 23 -1 -0.0272403 -1 0.0162146 163 0.1366231 25 28 222 0.1111111 26 27 -1 -0.000362931 -1 0.053106 -1 -0.0411224 29 179 -0.7777774 1 16 219 -1.549999 2 5 144 -0.1627895 3 4 -1 -0.0629469 -1 0.175678 111 -0.9999993 6 15 51 -0.7052494 7 8 -1 -0.0404534 165 -0.09008433 9 12 162 -1.743411 10 11 -1 0.203062 -1 0.029928 204 -0.3333332 13 14 -1 0.124407 -1 -0.0862302 -1 -0.042457 100 -1.1188 17 28 221 0.15 18 21 152 -2.085106 19 20 -1 0.121128 -1 -0.0113623 97 -1.608454 22 25 182 0.225 23 24 -1 0.0360956 -1 -0.0487822 135 -0.6041665 26 27 -1 0.160482 -1 0.049852 -1 4.77853e-05 29 151 -1.947368 1 4 103 -1.749999 2 3 -1"
   "-0.0595498 -1 0.0611172 13 -0.9999998 5 8 282 216 6 7 -1 -0.0290917 -1 0.0641978 6 -1.833319 9 12 99 -0.654628 10 11 -1 -0.0619442 -1 0.0966343 63 -0.4999995 13 14 -1 -0.0129247 206 0.6180554 15 28 98 0.9499924 16 25 102 0.02891709 17 18 -1 -0.00562756 191 0.6422482 19 22 222 -0.09523808 20 21 -1 0.000607867 -1 0.0235541 39 0.7499993 23 24 -1 -0.00236898 -1 -0.0904671 126 -1.125 26 27 -1 0.113303 -1 -0.0475528 -1 0.0208863 29 207 -2.904851 1 6 194 0.0755746 2 3 -1 -0.0805549 127 -0.7794116 4 5 -1 0.110934 -1 -0.0421268 93 -3.7813 7 8 -1 -0.0888865 263 -0.01630428 9 26 119 -0.2356028 10 15 282 112 11 14 188 -0.5974116 12 13 -1 -0.00856385 -1 -0.0405485 -1 0.00858984 128 -1.064516 16 21 153 -1.281553 17 18 -1 0.159599 216 0.07985317 19 20 -1 -0.0452354 -1 0.0600593 193 -0.844848 22 25 99 -0.3085434 23 24 -1 -0.0345356 -1 0.0679995 -1 -0.00169755 33 0.8865539 27 28 -1 -0.0101568 -1 0.00930508"
   "29 183 -0.4999998 1 28 105 -1.552631 2 7 63 -1.199999 3 4 -1 0.0504449 131 -0.3999997 5 6 -1 -0.00392362 -1 -0.158309 31 0.04650466 8 11 268 0.2844827 9 10 -1 0.0639299 -1 -0.135165 143 -1.252353 12 13 -1 -0.0700164 93 -1.59405 14 15 -1 0.0523357 174 -0.137931 16 27 169 0.2408186 17 26 108 0.5499996 18 23 194 0.3458102 19 20 -1 0.017079 148 1.027346 21 22 -1 -0.0580688 -1 0.109145 162 -0.3439344 24 25 -1 -0.0796959 -1 0.0453862 -1 0.0660828 -1 -0.0375686 -1 0.00232629 29 203 16.37493 1 26 264 0.9090902 2 23 133 0.4999993 3 18 115 0.5361898 4 9 152 -0.1041666 5 6 -1 0.00638059 281 134 7 8 -1 -0.0018504 -1 -0.0400834 280 0.8422939 10 13 149 -0.2142856 11 12 -1 -0.0261448 -1 0.0516882 45 0.7419354 14 15 -1 0.00964073 102 -0.3890566 16 17 -1 0.524981 -1 -0.00268243 130 0.09999992 19 22 177 -0.9999997 20 21 -1 0.1799 -1 -0.0314571 -1 0.00711407 248 -0.5833329 24 25 -1 0.0494319 -1 0.00665145 192"
   "-0.1468947 27 28 -1 0.0425963 -1 -0.104926 29 114 2.249999 1 28 14 2.874999 2 27 24 0.03178299 3 6 248 -0.2291666 4 5 -1 0.0188919 -1 -0.0724642 218 2.968749 7 26 25 1.123609 8 23 18 -0.999996 9 16 7 0.1107685 10 15 6 -0.5384582 11 14 200 1.447916 12 13 -1 -0.0222923 -1 0.153022 -1 0.0716401 -1 -0.0164924 38 0.8518518 17 22 282 15 18 19 -1 -0.0481679 196 -0.5333332 20 21 -1 0.0356687 -1 -0.00612249 -1 0.00527159 227 0.3295453 24 25 -1 -0.0715219 -1 0.0601155 -1 0.083668 -1 -0.0881638 -1 0.0778744 29 116 0.4818119 1 12 165 -0.20082 2 9 28 0.9147397 3 8 263 -0.01024521 4 7 280 0.6390977 5 6 -1 -0.0239789 -1 0.00423966 -1 0.00582363 -1 -0.0499581 101 -0.3861647 10 11 -1 -0.0107259 -1 0.00973765 139 0.8915143 13 24 139 -0.0800865 14 23 153 0.15625 15 20 180 -0.5277775 16 19 133 1.023809 17 18 -1 0.000174836 -1 0.344889 -1 -0.0433421 162 0.3893893 21 22 -1 0.307963 -1 -0.0441909 -1 -0.0547655"
   "222 -0.3726415 25 28 110 0.749997 26 27 -1 0.000312467 -1 0.315438 -1 -0.0178604 29 40 0.2777777 1 8 33 0.9969126 2 3 -1 0.00873865 263 -0.04960028 4 5 -1 0.00176647 30 0 6 7 -1 0.0485025 -1 -0.0422568 77 -0.4790978 9 14 26 0.07955339 10 13 39 0.2352941 11 12 -1 -0.00465293 -1 -0.116652 -1 0.0158357 26 0.3838101 15 16 -1 -0.0228192 195 -1.285714 17 18 -1 -0.0805132 48 0.2139906 19 28 15 0.4705882 20 27 176 -4.999998 21 22 -1 0.168371 25 0.4180715 23 26 218 -3.136362 24 25 -1 -0.14414 -1 0.0159257 -1 -0.040668 -1 0.113539 -1 -0.054617 29 127 -2.124996 1 2 -1 -0.078912 243 1.277777 3 22 228 1.529411 4 21 243 -1.407407 5 10 258 0.2465446 6 7 -1 -0.00360329 213 -0.04032227 8 9 -1 0.127114 -1 0.0304759 105 1.552631 11 18 226 93750 12 17 178 2.299998 13 16 168 1.138015 14 15 -1 -0.000817604 -1 -0.0569069 -1 0.0774973 -1 -0.0778527 213 0.2464105 19 20 -1 -0.00117528 -1 0.101107 -1 -0.0568356 264"
   "-1.333333 23 26 134 0.3624999 24 25 -1 0.140837 -1 -0.0173891 278 6.5 27 28 -1 -0.0181936 -1 0.0378866 29 282 194 1 16 41 0.05555554 2 11 248 -0.02678571 3 10 281 161 4 9 264 -0.2166666 5 8 90 -1.333333 6 7 -1 0.133684 -1 0.0065074 -1 -0.0260844 -1 0.0427241 -1 -0.0460753 187 -0.7068829 12 15 229 -1.522727 13 14 -1 -0.132916 -1 0.018331 -1 -0.00168272 219 -0.4374991 17 18 -1 0.036794 217 0.3002007 19 26 106 0.1249999 20 25 28 0.3676561 21 22 -1 0.0303468 26 0.6740275 23 24 -1 0.0146686 -1 -0.143128 -1 0.0243766 223 -0.3599999 27 28 -1 -0.0365115 -1 0.0836935 29 247 -0.04761904 1 6 36 0.8545454 2 3 -1 -0.00986941 44 0.5432595 4 5 -1 0.0533595 -1 0.00474771 245 0.2499999 7 28 56 0.799202 8 25 195 -0.09999994 9 10 -1 -0.006282 130 0.4999996 11 24 35 0.5384615 12 15 1 -0.003484036 13 14 -1 0.014587 -1 0.055036 90 1.117647 16 23 171 -0.456836 17 22 132 0.7631577 18 21 74 1.186705 19 20 -1 0.0174003"
   "-1 0.158031 -1 0.155038 -1 -0.00860695 -1 -0.0599624 -1 -0.0249558 71 -0.9440562 26 27 -1 -0.0234591 -1 0.0432748 -1 -0.0130685 29 30 0 1 16 107 -0.9230768 2 7 114 -1.043478 3 4 -1 -0.055868 200 0 5 6 -1 0.062701 -1 -0.00535103 23 0.008776813 8 9 -1 0.0505896 42 0.2666666 10 11 -1 -0.0636079 48 -0.2008159 12 15 2 -0.4880689 13 14 -1 -0.00637039 -1 0.116001 -1 -0.0607798 34 0.3142857 17 22 133 -0.3541665 18 19 -1 0.0281067 226 0.6499998 20 21 -1 0.00791263 -1 -0.0493715 149 -0.972222 23 24 -1 -0.0360903 32 0.2364602 25 26 -1 -0.0323172 225 -0.9374981 27 28 -1 -0.0503163 -1 0.00633733 29 241 0.2499999 1 18 31 1.090704 2 15 24 0.7469252 3 12 2 0.9288947 4 5 -1 -0.00190442 125 0.5152317 6 11 229 -0.7569443 7 10 263 -0.02231794 8 9 -1 -0.0365831 -1 0.552279 -1 0.0749221 -1 -0.0691844 77 -0.2668114 13 14 -1 -0.065495 -1 -0.0110437 167 -0.3194423 16 17 -1 0.125147 -1 0.0138819 79 1.09183 19 28 264"
   "0.2058823 20 25 130 0.1648936 21 22 -1 -0.000181629 181 0.2499999 23 24 -1 0.00734525 -1 0.0660909 28 0.173536 26 27 -1 0.132485 -1 0.0245141 -1 -0.0614054 29 100 -1.650277 1 2 -1 -0.0535319 151 -1.947368 3 4 -1 0.0467026 169 -1.295504 5 12 264 -2.499999 6 7 -1 0.119143 280 0.9481132 8 11 229 -1.666666 9 10 -1 -0.0759953 -1 0.0488075 -1 -0.0202114 13 -0.8461536 13 20 164 -0.0356665 14 17 213 -3.340406 15 16 -1 0.132958 -1 -0.0432642 178 -0.4827585 18 19 -1 0.130731 -1 -0.00476345 191 -3.415469 21 22 -1 0.059847 164 0.9765914 23 26 6 -2.206887 24 25 -1 0.103444 -1 -0.000203486 133 0.2499995 27 28 -1 -0.167775 -1 0.0320266 29 188 0.7486411 1 2 -1 0.000871386 106 1.062499 3 24 133 -0.6630434 4 9 30 0.09759956 5 6 -1 -0.0388808 136 -0.8285713 7 8 -1 -0.0764906 -1 0.128047 7 1.116324 10 23 217 0.01388819 11 12 -1 -0.0579735 189 0.9094833 13 16 204 1.999997 14 15 -1 -0.052167 -1 0.139955 126 0.4499999"
   "17 22 45 0.5376343 18 19 -1 -0.0412486 23 0.8627008 20 21 -1 0.183273 -1 -0.0370882 -1 -0.0434928 -1 0.10508 216 0.3199764 25 26 -1 -0.0116224 206 0.8139532 27 28 -1 0.272885 -1 -0.00312268 29 42 0 1 10 10 -1.319005 2 9 116 -1.356334 3 4 -1 0.0735436 110 -0.5294116 5 8 62 -0.7142853 6 7 -1 0.0880303 -1 -0.0850549 -1 0.0263697 -1 -0.0535182 16 -0.09090906 11 12 -1 0.00640906 221 0 13 14 -1 -0.0180334 131 0.06249998 15 16 -1 -0.0126874 36 0.7692307 17 24 198 -0.2771739 18 19 -1 -0.00193196 108 0.1249999 20 21 -1 0.00206807 201 -0.4499999 22 23 -1 0.108671 -1 0.0377782 171 0.3863448 25 28 244 -0.535714 26 27 -1 0.0576466 -1 -0.0320787 -1 0.102854 29 45 1.142857 1 20 48 -0.1598132 2 3 -1 0.00249766 178 1.249998 4 19 199 0.1424242 5 6 -1 -0.0186028 153 0.7499998 7 18 99 1.04315 8 15 70 0.1890041 9 10 -1 -0.00399053 186 0.1508621 11 14 223 0.3999994 12 13 -1 0.0228581 -1 0.347092 -1 0.0139878 229"
   "-0.3499999 16 17 -1 0.316539 -1 0.0396667 -1 -0.0489602 -1 0.0557417 46 -0.368962 21 22 -1 -0.008138 10 -2.601062 23 24 -1 -0.0479096 31 1.06988 25 26 -1 0.0373973 121 0.240962 27 28 -1 0.194309 -1 0.0146088 29 53 1.117635 1 28 209 0.6851734 2 17 18 -0.999996 3 8 198 -0.2386363 4 5 -1 -0.0165066 206 0.009803918 6 7 -1 0.0982021 -1 0.0211806 41 0 9 16 248 -0.02678571 10 11 -1 -0.00612999 128 -0.3599999 12 15 218 -0.4137929 13 14 -1 0.0535712 -1 -0.0409404 -1 -0.0700105 -1 0.000968041 281 162 18 27 190 0.9642802 19 26 25 0.2116089 20 21 -1 -0.0397187 122 1.063754 22 25 64 -0.698795 23 24 -1 -0.054692 -1 0.0544281 -1 -0.0817884 -1 -0.132361 -1 0.0980103 -1 -0.0260733 29 6 1.947354 1 28 246 -0.07499997 2 3 -1 -0.00485664 110 -0.025 4 13 237 -0.2184903 5 8 257 0.2212405 6 7 -1 0.00788124 -1 0.0665949 87 -0.1111111 9 10 -1 6.22013e-05 179 -0.1249999 11 12 -1 0.00911825 -1 -0.0345914 269 -0.08333322"
   "14 15 -1 -0.00111413 24 0.8842506 16 27 195 0.6363634 17 26 43 0.1818181 18 25 281 200 19 24 232 -0.05213133 20 21 -1 -0.0372506 49 0.1752086 22 23 -1 0.0655062 -1 -0.0290091 -1 -0.0767983 -1 0.053286 -1 -0.0267482 -1 -0.0244711 -1 -0.0516242 29 38 0.2264151 1 22 125 0.6716973 2 21 153 0.910714 3 20 171 -0.4900332 4 5 -1 0.00946405 215 -0.0289604 6 7 -1 -0.0200889 194 0.2060146 8 19 94 -1.033361 9 10 -1 0.0208708 177 0.3977272 11 14 185 0.06393737 12 13 -1 -0.0433333 -1 -0.00368288 1 -0.5848952 15 18 282 111 16 17 -1 -0.0414785 -1 0.0527607 -1 0.0385246 -1 0.0242257 -1 0.0778011 -1 0.0320773 199 0.2297297 23 24 -1 -0.0168254 163 -0.2727488 25 26 -1 0.0464387 215 0.3307788 27 28 -1 -0.035721 -1 0.0614025 29 247 -1.249999 1 10 222 -0.6277173 2 7 42 0.8461538 3 6 277 5.5 4 5 -1 -0.0232597 -1 0.150153 -1 0.192206 174 0.275641 8 9 -1 -0.0174759 -1 0.0591968 247 0.3124999 11 16 202 1.818179 12"
   "15 26 0.2570495 13 14 -1 0.00190435 -1 -0.00991451 -1 0.0895524 131 -0.1999998 17 22 31 0.9892119 18 21 236 0.5402276 19 20 -1 0.000788414 -1 0.0693895 -1 0.117742 260 0.2332708 23 28 112 0.2499998 24 27 19 0.4050632 25 26 -1 -0.00996538 -1 0.0397203 -1 -0.0311477 -1 0.02488 29 6 0.9914476 1 24 103 -1.472222 2 5 241 1.145833 3 4 -1 -0.0667502 -1 0.021323 151 -1.947368 6 7 -1 0.054235 49 0.2466304 8 21 141 0.150343 9 20 231 -0.0270429 10 15 191 0.216432 11 14 102 0.01836973 12 13 -1 0.00623058 -1 0.0343696 -1 -0.0165312 30 0.104101 16 17 -1 -0.0470804 256 -0.04752237 18 19 -1 -0.0168255 -1 0.0069067 -1 -0.0188505 53 -0.2752136 22 23 -1 -0.0393611 -1 0.018234 117 0.7315046 25 28 112 1.142857 26 27 -1 -0.01605 -1 0.0482882 -1 -0.142388 29 8 -2.310031 1 2 -1 -0.063486 8 -1.872506 3 6 129 -0.9687495 4 5 -1 0.11843 -1 -0.00907565 24 5.525552e-05 7 8 -1 -0.0609263 25 0 9 14 33 0.9946227 10 13 110"
   "0.4166661 11 12 -1 0.085457 -1 -0.0762942 -1 -0.0411475 228 0.2272726 15 16 -1 0.00227622 134 0.5208332 17 26 34 0.5121951 18 21 175 -1.124999 19 20 -1 0.126388 -1 -0.0242718 25 0.6658424 22 25 188 -0.5766981 23 24 -1 0.0795456 -1 0.0109026 -1 -0.00854013 18 -0.1599999 27 28 -1 -0.0494168 -1 -0.00147938 29 241 -0.2499998 1 10 268 0.4891304 2 7 242 -0.653846 3 4 -1 0.026423 153 0.1326531 5 6 -1 -0.0151737 -1 0.0125296 182 0.3220338 8 9 -1 -0.0392379 -1 0.00284105 182 -1.343749 11 12 -1 -0.0299526 53 -0.9308169 13 14 -1 0.0149756 157 -1.165517 15 20 120 -0.7789487 16 17 -1 0.0293695 13 -1.142857 18 19 -1 0.0168489 -1 -0.0687842 280 0.7797619 21 24 191 -0.7825727 22 23 -1 -0.020347 -1 0.0111319 129 -0.5795453 25 28 91 -0.7499998 26 27 -1 -0.0274684 -1 0.0830732 -1 -0.0513068 29 175 2.249998 1 24 227 1.708332 2 23 190 0.06451563 3 20 224 0.8333329 4 19 186 -0.8132831 5 14 193 -0.09605371 6 11"
   "6 0.5925906 7 10 194 0.2060146 8 9 -1 0.022796 -1 -0.0226177 -1 0.0722236 174 0.409836 12 13 -1 -0.00284687 -1 -0.0884119 156 1.333332 15 18 204 0.7499985 16 17 -1 -0.0148191 -1 0.0318397 -1 0.0908475 -1 0.0271738 203 16.37493 21 22 -1 0.00691022 -1 -0.0999639 -1 -0.04056 211 0.09700508 25 26 -1 -0.0442007 136 0.1086956 27 28 -1 0.179671 -1 0.0287971 29 188 -2.269782 1 2 -1 -0.0827488 121 1.947354 3 28 121 0.9451448 4 19 57 -1.463414 5 8 34 0.4999997 6 7 -1 -0.0617643 -1 0.00221645 130 0.4375 9 10 -1 -0.000439553 131 0 11 18 164 0.8757282 12 17 23 0.7909089 13 16 281 241 14 15 -1 -0.0218248 -1 0.148759 -1 0.0476429 -1 -0.167518 -1 0.0303875 130 0.3392856 20 25 215 -0.9806641 21 22 -1 0.0713108 25 0.3273253 23 24 -1 -0.0501242 -1 0.00516243 203 0.5249999 26 27 -1 -0.111638 -1 0.0842968 -1 0.0798547 29 218 2.968749 1 28 197 -0.4758064 2 19 192 -0.4235567 3 18 135 -0.3333332 4 9 195 -4.163336e-17"
   "5 8 79 0.6060538 6 7 -1 -0.0537559 -1 0.0105762 -1 0.0221948 204 0.1319444 10 17 114 -0.3214284 11 12 -1 -0.0074025 166 -0.2541816 13 14 -1 0.0708532 217 -0.442059 15 16 -1 0.0730408 -1 -0.00431584 -1 -0.0341369 -1 -0.037692 10 0.4557588 20 25 209 0.6605638 21 24 280 0.1171171 22 23 -1 0.034655 -1 0.00553116 -1 -0.0250449 174 0.6136361 26 27 -1 -0.0121918 -1 0.0284486 -1 -0.0744366 29 245 -0.02941176 1 28 277 6.5 2 25 104 -0.1978022 3 8 237 -0.771512 4 5 -1 0.016925 39 0.02777777 6 7 -1 -0.0411998 -1 -0.00567746 28 0.4099217 9 14 253 1.061103 10 13 199 0.5056818 11 12 -1 0.0458196 -1 -0.0337684 -1 -0.0421124 38 0.2833333 15 22 255 -0.02173084 16 19 170 -0.1604512 17 18 -1 -0.00475474 -1 0.0505159 223 0.4345237 20 21 -1 -0.0116566 -1 0.0195955 217 -0.02310072 23 24 -1 -0.0469791 -1 0.0137773 237 -0.5032773 26 27 -1 -0.0692008 -1 -0.00990571 -1 0.00468339 29 268 -0.06944443 1 4 162 -1.329892"
   "2 3 -1 -0.0388386 -1 0.0120726 2 -0.4094949 5 18 15 -0.7703703 6 7 -1 -0.0023535 156 -0.4999998 8 13 151 -0.3888888 9 10 -1 0.00359106 227 -0.08333322 11 12 -1 0.148214 -1 0.0430305 204 -0.6666665 14 15 -1 -0.0688205 210 0.1563032 16 17 -1 -0.00377104 -1 0.0358822 110 -0.225 19 22 16 -0.5714282 20 21 -1 0.0382852 -1 -0.0431701 26 0.1959959 23 28 174 -0.5714285 24 25 -1 -0.0659995 105 -0.7419354 26 27 -1 0.116642 -1 0.0219088 -1 -0.0122338 29 182 1.657894 1 28 79 0.8066242 2 25 233 -0.2639627 3 6 277 2.5 4 5 -1 -0.0109087 -1 0.0218948 38 0.3090909 7 14 216 -0.8127826 8 13 215 -1.128059 9 10 -1 -0.0370482 228 -1.129411 11 12 -1 0.249977 -1 0.0378563 -1 -0.0400125 201 -0.2321427 15 18 256 -0.09018101 16 17 -1 -0.0137345 -1 0.0247884 82 -0.8347825 19 20 -1 -0.033454 221 -0.3214284 21 24 229 -0.5434782 22 23 -1 -0.0039649 -1 0.0353403 -1 -0.00805182 88 -0.3508771 26 27 -1 0.0178142 -1 -0.0355527"
   "-1 0.0511452 29 206 2.01282 1 26 198 0.8902436 2 19 201 0.1249998 3 10 210 0.7946184 4 9 41 0.04545454 5 8 219 -2.142856 6 7 -1 -0.138574 -1 0.0106512 -1 -0.00741722 -1 -0.0735045 200 1.218749 11 12 -1 0.00301127 19 0.4897958 13 18 167 0.7857098 14 17 188 0.6512435 15 16 -1 0.0571641 -1 -0.0469531 -1 0.179885 -1 -0.0222073 201 0.3124997 20 23 39 0.05555554 21 22 -1 0.0390704 -1 -0.0328049 158 -0.253012 24 25 -1 0.0545652 -1 -0.0777681 22 1.624999 27 28 -1 0.144491 -1 -0.0383681 29 65 0.9090906 1 18 226 0.7874997 2 15 34 0.5263157 3 10 30 0 4 7 119 0.389181 5 6 -1 -0.0289798 -1 0.037931 136 -1.338983 8 9 -1 -0.0344356 -1 0.0130468 147 -1.094564 11 12 -1 0.0366037 195 -1.160714 13 14 -1 -0.0571052 -1 -0.00702786 28 0.2778351 16 17 -1 0.0271796 -1 -0.0438358 127 -2.124996 19 20 -1 0.155989 197 0.4919354 21 28 24 0.4477632 22 25 38 0.1833333 23 24 -1 0.0251927 -1 -0.0261055 39 0.1666666 26 27"
   "-1 -0.0628568 -1 0.0741369 -1 -0.0205005 29 175 -1.999998 1 2 -1 -0.0633348 226 0.1999998 3 4 -1 0.00372803 227 0.8281246 5 28 217 0.1720991 6 27 153 -3.249999 7 8 -1 0.0874166 109 -0.1111111 9 16 145 -0.7367321 10 11 -1 0.0130654 131 -0.7499996 12 13 -1 0.0775819 212 -1.741505 14 15 -1 0.142332 -1 -0.036132 153 0.6274508 17 26 282 104 18 25 138 0.3946813 19 20 -1 0.0211924 213 0.0666664 21 22 -1 0.00280421 280 0.2465753 23 24 -1 0.370637 -1 0.102741 -1 -0.0129138 -1 -0.0497062 -1 -0.0235388 -1 -0.0414647 29 252 0.1730769 1 28 37 0.5999998 2 25 73 0.2019927 3 24 102 0.2411699 4 7 134 -0.2187499 5 6 -1 0.0125844 -1 -0.0141893 191 0.4990387 8 23 194 0.5098729 9 20 185 -0.1331731 10 11 -1 0.0516266 131 -0.04999996 12 17 229 -0.9999997 13 14 -1 -0.0430807 50 -0.08284724 15 16 -1 0.0459551 -1 0.170905 137 -0.486842 18 19 -1 -0.0475898 -1 0.0172307 39 0.5454543 21 22 -1 -0.0446992 -1 0.0400643"
   "-1 -0.0271545 -1 -0.0207176 97 -0.8085545 26 27 -1 -0.0495789 -1 0.0146531 -1 -0.00683281 29 92 0.2629528 1 20 93 -0.0364264 2 5 204 0.3928569 3 4 -1 0.00133888 -1 -0.019358 70 -0.6037902 6 7 -1 -0.0251118 59 -0.8333331 8 9 -1 0.0677229 188 -1.290525 10 11 -1 0.0861569 43 0 12 15 117 0.1735539 13 14 -1 0.00557508 -1 0.076634 26 0.9557858 16 17 -1 0.0138616 224 0.5446427 18 19 -1 -0.0307768 -1 0.0436819 184 0.7837237 21 28 217 -18.34512 22 23 -1 0.0918731 277 1.5 24 25 -1 0.00383217 230 0.4076474 26 27 -1 -0.0356161 -1 0.0735612 -1 0.036511 29 178 0.1785714 1 2 -1 0.00322834 177 0.06249994 3 6 112 -0.3437498 4 5 -1 0.0101943 -1 -0.0411099 112 0.2708332 7 14 122 -0.9054986 8 9 -1 0.0289925 281 78 10 13 241 -0.6176469 11 12 -1 -0.0534551 -1 0.0129745 -1 -0.0400361 70 0.03122619 15 28 157 1.035714 16 27 282 44 17 22 189 -0.4986008 18 19 -1 0.00889723 225 -0.2741934 20 21 -1 -0.0700195 -1 0.138589"
   "177 1.249999 23 26 40 0.1754385 24 25 -1 0.0440618 -1 -0.0159775 -1 0.102937 -1 -0.0472431 -1 -0.0271466 29 173 -0.5833329 1 12 189 -0.9895608 2 7 154 -0.08333331 3 4 -1 -0.00426175 281 155 5 6 -1 0.129286 -1 -0.0710262 32 1.130547 8 11 107 -4.749998 9 10 -1 0.0799759 -1 -0.0303496 -1 0.0817703 203 16.37493 13 28 281 34 14 27 150 -0.1249999 15 22 141 -0.05004606 16 19 49 -1.225061 17 18 -1 -0.0603111 -1 0.0823795 199 0.1556886 20 21 -1 0.0398713 -1 -0.0559905 194 -0.3979419 23 26 50 0.8516153 24 25 -1 0.0148 -1 0.135494 -1 -0.00993311 -1 2.56656e-05 -1 -0.069168 29 281 91 1 2 -1 -0.0040077 282 31 3 12 4 -0.4147805 4 9 203 0.3749999 5 8 94 0.08982711 6 7 -1 0.0581239 -1 0.00801122 -1 -0.0226927 1 0.6497734 10 11 -1 -0.0235241 -1 -0.166596 201 0.2692304 13 18 27 0.5321706 14 15 -1 0.0503398 222 0.2017045 16 17 -1 0.0218368 -1 -0.0420851 194 0.003445569 19 22 279 7.5 20 21 -1 -0.0290156 -1 0.0304658"
   "225 0.9583327 23 28 2 0.9288947 24 25 -1 -0.0238425 156 0.4374996 26 27 -1 0.0258107 -1 -0.45548 -1 -0.191169 29 207 -2.904851 1 4 194 0.06412304 2 3 -1 -0.0781917 -1 0.0169149 207 -1.112763 5 20 203 0.3928569 6 15 197 -0.1964285 7 8 -1 -0.0159709 223 1.187499 9 14 131 -0.749997 10 11 -1 -0.0360649 176 -0.2549019 12 13 -1 0.0552713 -1 0.00403181 -1 0.145631 113 -0.8706895 16 17 -1 0.0686722 11 -2.066666 18 19 -1 0.115963 -1 -0.0526311 213 -1.784991 21 24 132 -0.4583332 22 23 -1 -0.116139 -1 -0.0237349 38 0.07444168 25 28 111 -0.5681816 26 27 -1 -0.0400913 -1 -0.00225122 -1 0.00191885 29 65 2.15873 1 28 264 -1.629629 2 3 -1 -0.0295058 125 -0.7824329 4 5 -1 0.00829918 141 0.3023856 6 25 117 -6.775773 7 8 -1 0.107856 176 0.8305084 9 22 196 -1.145833 10 15 15 -0.7703703 11 14 93 -1.027554 12 13 -1 -0.00241456 -1 0.137682 -1 -0.0192507 227 0.1249999 16 19 28 0.8358375 17 18 -1 -0.0144781 -1 -0.0557654"
   "195 -1.247934 20 21 -1 -0.0448392 -1 0.000889484 6 0.7941144 23 24 -1 0.00802972 -1 0.174113 192 0.6249112 26 27 -1 0.027849 -1 -0.0462349 -1 0.0247646 29 168 -7.415331 1 2 -1 -0.0835841 255 -1.191135 3 12 259 0.861697 4 9 158 0.06249998 5 6 -1 0.062608 164 0.248257 7 8 -1 -0.0628513 -1 0.0687328 233 -0.120071 10 11 -1 -0.019023 -1 0.226216 24 0.778299 13 14 -1 0.00172188 107 -0.06617646 15 20 281 14 16 19 136 0.1818181 17 18 -1 -0.0244046 -1 0.376259 -1 -0.0401117 117 0.1372677 21 28 200 0.1363636 22 23 -1 -0.0135341 275 0.5609754 24 25 -1 0.0279922 40 0.7272726 26 27 -1 0.289153 -1 0.0489725 -1 -0.0257725 29 87 0.3333329 1 20 207 0.3587271 2 19 196 1.333333 3 18 282 7 4 5 -1 -0.0448249 161 0.699249 6 17 141 0.150343 7 10 247 -0.04999996 8 9 -1 0.0137865 -1 -0.00292926 49 0.2466304 11 12 -1 -0.0241191 107 0.9999999 13 16 53 -0.05139519 14 15 -1 -0.0352001 -1 0.0313873 -1 -0.0733812 -1 -0.0725924"
   "-1 0.0602167 -1 -0.0206257 31 0.4875278 21 26 281 165 22 23 -1 -0.0201626 162 -0.6388225 24 25 -1 -0.0822045 -1 0.044258 26 0.9604387 27 28 -1 0.00468837 -1 0.0313339 29 23 0.9595593 1 24 4 0.1327053 2 7 28 0.8732518 3 4 -1 -0.00122317 87 -0.6249997 5 6 -1 0.0790975 -1 -0.0666789 33 0.8330814 8 9 -1 -0.0169761 3 -1.590243 10 13 225 0.5249998 11 12 -1 -0.0565983 -1 0.0867123 2 -0.9571565 14 19 102 -1.030372 15 16 -1 -0.0243254 220 0.2109374 17 18 -1 0.10714 -1 0.0108135 200 0.1249998 20 23 177 0.01666666 21 22 -1 0.0548458 -1 0.00845256 -1 -0.00472344 237 -0.08796277 25 28 3 0.525728 26 27 -1 -0.0092056 -1 0.090048 -1 -0.0410101 29 63 -0.2926828 1 28 27 0.2980376 2 13 9 -0.657058 3 8 89 -1.374999 4 7 131 -0.7499996 5 6 -1 0.046213 -1 -0.0983125 -1 0.00665798 251 -0.3888887 9 10 -1 0.0513083 191 0.2302805 11 12 -1 -0.0781765 -1 0.0348118 219 -0.9531245 14 19 23 0.4818563 15 18 151 -0.9999998"
   "16 17 -1 -0.0499497 -1 0.104188 -1 -0.048435 282 63 20 21 -1 0.0176695 165 0.3720335 22 25 196 0.7272725 23 24 -1 -0.00769055 -1 -0.0574011 221 0.5468749 26 27 -1 0.0280394 -1 -0.0177176 -1 -0.00480561 29 221 -1.714285 1 2 -1 -0.0650585 244 -1.249995 3 10 128 0.1323529 4 7 121 -0.7258027 5 6 -1 -0.0852365 -1 0.0178307 41 0 8 9 -1 0.123635 -1 0.0142057 127 -2.124996 11 12 -1 -0.0874889 133 0.4999993 13 24 178 0.1999998 14 21 111 -1.135714 15 18 161 0.1747763 16 17 -1 0.0278198 -1 -0.033162 7 -0.9207646 19 20 -1 -0.0254936 -1 -0.000141571 216 0.8357174 22 23 -1 0.0129764 -1 -0.0350946 130 0.09999992 25 28 177 -0.999998 26 27 -1 0.15816 -1 -0.0254066 -1 0.00792154 29 31 0.1874086 1 22 281 226 2 21 219 -2.142856 3 4 -1 0.144404 5 -0.401389 5 16 150 -1.2 6 9 4 -0.8584518 7 8 -1 -0.0490025 -1 0.119128 140 0.1705012 10 15 211 -1.203962 11 14 120 -1.459194 12 13 -1 -0.0458055 -1 0.13924 -1 -0.0343841"
   "-1 -0.0749099 215 -0.2772803 17 18 -1 0.109997 41 0.499999 19 20 -1 -0.0762948 -1 0.0471211 -1 0.0811743 238 0.1755717 23 24 -1 0.00377643 86 -0.1666666 25 26 -1 -0.0246791 237 0.07371289 27 28 -1 -0.00953689 -1 0.0237933 29 197 1.567307 1 24 197 0.8708332 2 7 187 0.3900655 3 4 -1 0.0010759 200 1.166665 5 6 -1 -0.023947 -1 0.0379874 184 0.4724094 8 13 165 1.102866 9 12 206 0.2435896 10 11 -1 0.0249037 -1 -0.0630176 -1 0.201721 203 0.2797619 14 23 104 0.5555553 15 22 196 0.5882351 16 21 82 0.9142855 17 20 278 8.5 18 19 -1 0.441709 -1 0.0902839 -1 -0.0423239 -1 0.101835 -1 -0.026773 -1 -0.00596696 280 0.9130435 25 26 -1 -0.0530053 227 -0.3555554 27 28 -1 -0.0841456 -1 0.146082 29 37 0.15 1 2 -1 0.00963959 27 0.2980376 3 6 153 -1.103448 4 5 -1 0.0314646 -1 -0.0373051 36 0.01724138 7 8 -1 -0.0412322 217 0.1019647 9 26 223 0.4999995 10 11 -1 -0.00794142 156 -0.3571427 12 17 70 -0.5909525 13 14"
   "-1 0.0112501 25 0.3919926 15 16 -1 0.0864853 -1 -0.0692507 206 1.034091 18 25 153 -0.678571 19 24 177 0.499999 20 21 -1 -0.0082076 138 -0.3921703 22 23 -1 0.0301812 -1 0.126574 -1 -0.00598147 -1 -0.094258 148 -2.114867 27 28 -1 0.0903512 -1 0.0130238 29 121 -1.086325 1 4 6 -0.05555537 2 3 -1 -0.030802 -1 0.0357219 97 -1.004719 5 14 221 0.1704545 6 7 -1 -0.00222993 219 0.8999971 8 13 171 -2.097923 9 10 -1 0.161796 128 -0.1666666 11 12 -1 0.065152 -1 0.0041288 -1 -0.0291805 58 -0.4761903 15 16 -1 -0.0114787 223 0.3749996 17 28 218 0.5833332 18 27 214 0.8425933 19 26 23 0.200185 20 21 -1 0.0448966 55 -0.05042036 22 23 -1 -0.0112211 197 1.734723e-18 24 25 -1 0.0289728 -1 -0.000983089 -1 0.0717198 -1 -0.0191384 -1 -0.00593035 29 200 218750 1 26 134 562500 2 25 282 194 3 20 41 0.04545454 4 11 248 -0.02678571 5 10 126 0.06521738 6 7 -1 0.0156497 271 0.1874999 8 9 -1 -0.0567772 -1 0.00285357 -1 -0.0460825"
   "151 1.549999 12 19 19 1.071428 13 18 201 0.15625 14 17 225 -0.8749995 15 16 -1 -0.0466859 -1 0.00963955 -1 -0.00567403 -1 -0.0241027 -1 0.0648017 204 1.999997 21 24 206 0.4545454 22 23 -1 0.007612 -1 0.0699126 -1 -0.112371 -1 0.0676269 125 0.4209118 27 28 -1 0.00561883 -1 0.139974 29 221 1.062499 1 24 227 0.7941175 2 11 214 -1.01728 3 4 -1 -0.0169416 214 -0.9639091 5 10 218 -3.136362 6 7 -1 0.11983 174 -0.4782608 8 9 -1 -0.0358194 -1 0.0503544 -1 -0.000116059 168 -2.317461 12 13 -1 0.0900426 154 -0.1666664 14 19 280 0.4897119 15 18 279 15.5 16 17 -1 0.141198 -1 0.0249084 -1 -0.0143116 162 0.7614007 20 23 174 0.4374999 21 22 -1 0.00451293 -1 -0.0560611 -1 0.0980852 282 119 25 26 -1 -0.0449156 36 0.9417039 27 28 -1 0.0743245 -1 -0.0771599 29 200 93750 1 26 199 -4.349998 2 3 -1 -0.0563139 186 -1.351333 4 7 195 -0.3421051 5 6 -1 0.0447473 -1 -0.0204982 187 -0.8875519 8 17 151 0.3541665 9 16 27"
   "0.2762305 10 15 101 -0.6686478 11 14 161 -1.068166 12 13 -1 0.0607176 -1 -0.0344093 -1 -0.0884731 -1 0.00476926 -1 -0.0547061 185 -0.6691351 18 23 125 -0.3162209 19 20 -1 0.0532826 182 -0.09615382 21 22 -1 -0.04517 -1 0.0360433 195 -1.749999 24 25 -1 -0.0863393 -1 0.000611789 19 -1.192982 27 28 -1 0.0241817 -1 -0.0717453 29 274 -1.840908 1 8 34 0.7662082 2 5 86 0.4166665 3 4 -1 -0.0235352 -1 0.104371 102 0.0841831 6 7 -1 0.0226488 -1 0.124137 213 -0.559025 9 28 256 0.1932289 10 27 102 -0.1754497 11 12 -1 -0.0477427 6 -0.153005 13 18 163 0.8734089 14 15 -1 -0.0436521 220 -0.9402515 16 17 -1 0.305232 -1 0.00429001 72 -0.01830193 19 26 111 0.7083329 20 25 85 -0.09302322 21 22 -1 0.0121755 279 15.5 23 24 -1 0.182402 -1 0.0414388 -1 -0.0500409 -1 -0.052776 -1 0.0143431 -1 0.00125896 29 250 0.9062495 1 24 16 -0.6666664 2 7 155 -0.4285712 3 4 -1 -0.0568424 106 1.749993 5 6 -1 -0.00931148 -1 0.178718"
   "242 -1.874996 8 9 -1 -0.062367 221 1.014705 10 17 202 0.04545453 11 14 282 7 12 13 -1 -0.0445924 -1 0.00947206 184 -0.566219 15 16 -1 -0.0456327 -1 -0.000972293 149 -0.8928566 18 19 -1 -0.109371 49 -0.8678002 20 23 139 -0.8202509 21 22 -1 -0.0307752 -1 0.211684 -1 0.029697 7 -1.011945 25 28 255 -0.2386488 26 27 -1 -0.0931745 -1 0.0394919 -1 -0.0228297 29 234 -0.4596546 1 2 -1 -0.0126028 238 -0.3439973 3 4 -1 0.0187768 15 0.4473684 5 24 223 -0.06249994 6 9 186 0.3696042 7 8 -1 -0.0120369 -1 0.0237937 213 -0.3187475 10 13 198 0.6428568 11 12 -1 0.0264427 -1 -0.0377202 221 -0.7499998 14 15 -1 0.0599272 96 -0.4357008 16 19 147 -0.7464221 17 18 -1 0.00817079 -1 -0.0306362 198 -0.9814812 20 21 -1 0.0839872 281 226 22 23 -1 0.00329058 -1 0.104464 214 1.222334 25 26 -1 -0.0172675 226 -0.2794117 27 28 -1 -0.0704994 -1 0.0934408 29 36 1.161826 1 26 197 0.9333332 2 19 36 0.7962962 3 18 112 0.419753"
   "4 17 37 0.15 5 10 255 0.058953 6 9 110 -0.5999995 7 8 -1 -0.00564611 -1 0.0394606 -1 -0.00845775 36 0.01724138 11 12 -1 -0.0529794 27 0.3518881 13 16 220 0.3984374 14 15 -1 -0.0317938 -1 0.0334375 -1 0.00116328 -1 0.0153862 -1 -0.0106617 213 -0.004237259 20 25 75 -1.830496 21 22 -1 0.0678695 174 0.06428571 23 24 -1 -0.118434 -1 -0.0243669 -1 0.00484362 282 59 27 28 -1 0.0620789 -1 -0.00384592 29 243 0.3333332 1 28 34 0.7142856 2 13 88 0.2999999 3 4 -1 -0.00170071 38 0.5337078 5 6 -1 0.00963434 203 -0.4062498 7 8 -1 0.143778 225 -0.4999999 9 12 125 0.06790631 10 11 -1 -0.00696844 -1 0.272684 -1 0.0159786 228 0.4406778 14 27 186 0.9057844 15 22 197 0.1333333 16 21 27 0.4807364 17 20 208 0.721491 18 19 -1 -0.0454452 -1 0.188012 -1 0.0121829 -1 -0.0251092 128 -0.1999999 23 26 107 0.829268 24 25 -1 0.231384 -1 -0.0382435 -1 0.00811097 -1 -0.0315807 -1 0.00822229 29 281 90 1 8 204 -0.125 2 3 -1"
   "-0.0184493 121 0.7364295 4 5 -1 0.00365376 280 0.646789 6 7 -1 -0.0625456 -1 0.00514273 203 0.01136363 9 12 199 0.5982141 10 11 -1 0.0184359 -1 -0.0367586 35 0.4999998 13 20 92 -0.06386985 14 15 -1 -0.00274523 83 -0.4999998 16 17 -1 -0.0998035 105 -0.1749999 18 19 -1 -0.0830714 -1 -0.00483367 185 -0.2788006 21 26 191 0.5375689 22 23 -1 0.00185164 155 -0.1249998 24 25 -1 -0.0200667 -1 -0.177689 28 0.5591182 27 28 -1 0.0649737 -1 0.00802614 29 6 1.947354 1 28 137 -0.3249999 2 15 282 111 3 6 263 -0.01024521 4 5 -1 -0.0285967 -1 -0.000831968 225 -0.367647 7 12 40 0.468085 8 11 182 -0.7525772 9 10 -1 -0.0147211 -1 0.0418172 -1 -0.0319145 215 -0.3730168 13 14 -1 0.0521703 -1 0.00384781 193 -0.6874389 16 25 140 -0.8804026 17 18 -1 -0.0346426 281 233 19 24 162 0.03100121 20 23 282 99 21 22 -1 0.0486985 -1 -0.0554405 -1 0.0037425 -1 -0.106084 111 0.6904761 26 27 -1 0.00275695 -1 -0.0212034 -1 -0.0500968"
   "29 245 -0.02941176 1 22 277 6.5 2 19 238 -0.5043467 3 8 6 0.6904749 4 7 271 -0.7499994 5 6 -1 -0.0904234 -1 0.00991523 -1 0.0572453 136 1.36842 9 16 38 0.2264151 10 13 30 0.009517537 11 12 -1 -0.0138404 -1 0.0108086 240 -0.6730031 14 15 -1 0.0262403 -1 -0.0306074 219 0.9218745 17 18 -1 -0.0923647 -1 0.0882111 237 -0.5032773 20 21 -1 -0.0653624 -1 -0.00908663 277 2.5 23 28 237 -0.003922204 24 27 130 -0.4799999 25 26 -1 -0.00796653 -1 0.0483905 -1 -0.0128322 -1 0.00976733 29 104 1.491525 1 20 206 1.410713 2 15 241 0.2631578 3 4 -1 -0.00280909 127 0.999998 5 12 264 0.4999995 6 11 228 0.1911764 7 8 -1 0.0101461 281 26 9 10 -1 0.0543485 -1 -0.0139935 -1 0.0353724 240 -1.005807 13 14 -1 -0.0524948 -1 0.0476402 113 -0.5799999 16 17 -1 0.0569371 155 0.3499997 18 19 -1 -0.101598 -1 0.0282261 217 -0.8583064 21 22 -1 -0.0481391 95 -0.5200236 23 26 98 0.9774389 24 25 -1 -0.0118354 -1 0.0898311 199 -0.3749999"
   "27 28 -1 0.155936 -1 0.048216 29 194 -2.173675 1 10 103 0.03124998 2 7 224 0.6249975 3 6 18 -0.4999997 4 5 -1 -0.0831432 -1 -0.00554113 -1 -0.144018 187 0.0332526 8 9 -1 0.0272785 -1 -0.0551799 221 1.014705 11 26 208 0.6616796 12 19 145 -1.040493 13 18 161 -0.4537453 14 17 135 1.435897 15 16 -1 -0.0454115 -1 0.109975 -1 0.0487794 -1 -0.000374399 224 0.3333329 20 23 97 0.05459847 21 22 -1 -0.120859 -1 -0.0303024 33 0.9571081 24 25 -1 -0.0237542 -1 0.058967 213 1.754451 27 28 -1 0.0258707 -1 0.214008 29 6 -0.2272711 1 28 45 0.6562499 2 19 131 -0.2499995 3 14 194 0.5098729 4 13 144 -0.6082439 5 12 138 -0.7414291 6 7 -1 -0.0131045 194 0.3793507 8 11 214 0.1710713 9 10 -1 -0.0777423 -1 0.0334823 -1 -0.150928 -1 -0.00477809 -1 0.0204779 90 -0.7499993 15 16 -1 0.0297325 196 -0.833333 17 18 -1 -0.0463398 -1 0.00389604 108 -0.1351351 20 21 -1 0.0460715 281 144 22 25 156 -0.173913 23 24 -1 -0.0540925"
   "-1 0.00553581 59 -0.3749999 26 27 -1 0.10717 -1 0.00429908 -1 -0.00360503 29 241 -0.2499998 1 8 138 0.4085441 2 3 -1 -0.0103184 196 1.333333 4 7 188 -0.1363789 5 6 -1 0.0901957 -1 0.0106539 -1 -0.138476 107 -0.7118643 9 14 72 -0.6754593 10 11 -1 0.00299886 7 -0.1890177 12 13 -1 0.043158 -1 -0.0670437 243 0.8499997 15 28 141 0.3023856 16 27 15 -0.5736433 17 20 281 206 18 19 -1 -0.0296145 -1 0.0857343 173 0.4999999 21 24 99 0.06676751 22 23 -1 0.0118115 -1 -0.0334887 203 0.025 25 26 -1 -0.000485847 -1 -0.0626057 -1 0.0190778 -1 0.0346217 29 280 0.05309735 1 12 165 -0.9811883 2 7 177 -0.06249994 3 6 222 -0.3027778 4 5 -1 -0.0598792 -1 0.124932 -1 -0.0548838 199 0.708333 8 11 215 0.4410724 9 10 -1 -0.0544072 -1 0.0715295 -1 0.103704 217 0.7932674 13 22 23 0.2791596 14 19 193 0.2911964 15 16 -1 0.00262606 220 0.2554347 17 18 -1 0.0523777 -1 -0.00764072 98 -0.5980345 20 21 -1 -0.0238912 -1 0.000273139"
   "207 -0.3910342 23 26 248 0.2592592 24 25 -1 -0.0326243 -1 0.117729 212 -0.393634 27 28 -1 0.260458 -1 -0.0107975 29 36 1.25 1 26 180 562500 2 25 121 1.947354 3 22 6 0.9914476 4 19 118 0.3287993 5 8 18 1.599999 6 7 -1 -4.29339e-05 -1 -0.0405343 122 -0.4037736 9 10 -1 -0.0678669 256 0.3126259 11 18 122 0.9109604 12 15 26 1.013532 13 14 -1 0.0492909 -1 -0.032084 186 0.795228 16 17 -1 0.0191239 -1 -0.0894183 -1 -0.0102646 117 0.7315046 20 21 -1 -0.0108977 -1 -0.11615 40 0.4999998 23 24 -1 -0.0408633 -1 0.126498 -1 0.0698825 75 -1.475721 27 28 -1 0.0911268 -1 -0.0488205 29 23 0.9843697 1 26 8 1.060394 2 23 1 -0.06605526 3 14 204 0.3984374 4 7 231 0.1222818 5 6 -1 0.00313942 -1 -0.021003 121 -0.1485139 8 13 167 -0.8012391 9 10 -1 -0.0439343 198 0.2624999 11 12 -1 -0.0229239 -1 0.0418378 -1 -0.0371302 35 0.62 15 20 82 -0.8347825 16 17 -1 0.0659768 165 0.56626 18 19 -1 0.0070556 -1 0.0537058 28 0.3374137"
   "21 22 -1 -0.0777615 -1 -0.00302323 281 73 24 25 -1 -0.0340448 -1 0.0757585 85 1.833332 27 28 -1 -0.0303047 -1 0.0803982 29 70 0.03122619 1 14 15 0.2 2 5 143 0.6266369 3 4 -1 -0.00100858 -1 0.0176632 33 0.9222661 6 7 -1 0.103801 266 0.7499998 8 13 80 0.06666666 9 12 26 0.7041351 10 11 -1 0.0590564 -1 -0.052993 -1 -0.0591506 -1 0.111637 100 -0.3481039 15 16 -1 0.0218282 222 -6.642853 17 18 -1 0.238496 129 -0.3636362 19 20 -1 0.0255004 18 0.5714282 21 22 -1 -0.0460042 25 0.2604601 23 28 24 0.8115387 24 25 -1 -0.00394774 121 0.1481474 26 27 -1 -0.0409897 -1 0.185057 -1 -0.0545825 29 185 0.7363017 1 24 201 0.7352938 2 3 -1 -8.59432e-05 150 -0.04166664 4 19 103 0.6749997 5 18 46 -0.9529931 6 7 -1 -0.0171811 145 0.651932 8 17 93 0.2054884 9 10 -1 0.0844575 120 0.338538 11 16 181 0.1190476 12 13 -1 0.0473776 102 -0.2246159 14 15 -1 0.0158501 -1 0.607484 -1 0.0035721 -1 -0.068204 -1 -0.059944 6 0.5294087"
   "20 23 144 -0.1111106 21 22 -1 -0.0382942 -1 0.0357527 -1 -0.050449 102 -2.452609 25 26 -1 0.0828921 190 -0.7704894 27 28 -1 0.132915 -1 -0.0375517 29 257 -1.488086 1 4 233 -0.2182788 2 3 -1 0.0752624 -1 -0.0238807 86 -0.1276595 5 16 223 -0.4078946 6 15 44 0.2932551 7 10 14 -0.5490194 8 9 -1 -0.00326727 -1 0.0857074 169 -0.1802279 11 12 -1 -0.0227975 198 -0.3985506 13 14 -1 -0.0769071 -1 0.064118 -1 -0.0100219 187 -0.9378403 17 20 133 -0.1630434 18 19 -1 -0.0263833 -1 0.0384254 176 -0.2863636 21 28 282 190 22 27 223 -0.8749965 23 24 -1 -0.113678 82 -0.3846153 25 26 -1 -0.0453226 -1 -0.00582702 -1 0.0414677 -1 0.00592671 29 161 -3.004459 1 6 194 0.07003272 2 3 -1 -0.087926 187 0.06513957 4 5 -1 0.095738 -1 -0.0462402 168 -5.884903 7 10 66 -0.1428571 8 9 -1 -0.0215318 -1 0.111614 7 -2.906965 11 20 195 1.375 12 19 178 -0.7499993 13 16 156 0.2499998 14 15 -1 -0.0426677 -1 0.108647 193 -0.5580077"
   "17 18 -1 0.00726379 -1 -0.0744538 -1 0.124819 197 1.567307 21 26 206 1.609374 22 23 -1 0.000754163 197 0.08839778 24 25 -1 -0.0490521 -1 0.0999705 94 0.4075548 27 28 -1 -0.069114 -1 0.0486778 29 85 -1.53846 1 2 -1 0.0499755 151 -0.9999998 3 4 -1 -0.0130585 151 -0.2499999 5 8 143 -1.632151 6 7 -1 0.0694963 -1 0.00773522 143 -0.2928874 9 14 205 -0.5240384 10 11 -1 0.0205012 36 1.104167 12 13 -1 -0.0310116 -1 0.0285996 282 41 15 24 280 0.3875598 16 21 125 -0.6317593 17 18 -1 0.0515932 119 -0.5641683 19 20 -1 0.0667685 -1 -0.015489 26 0.9845922 22 23 -1 0.0696499 -1 0.362653 206 -0.2708332 25 26 -1 -0.0359953 197 0.899999 27 28 -1 0.00355442 -1 -0.0433046 29 11 1.666664 1 14 175 -1.999998 2 3 -1 -0.0755611 131 0.04999996 4 5 -1 0.00426945 36 0.3636363 6 11 92 0.8135559 7 10 213 -0.1907204 8 9 -1 0.0375323 -1 -0.00254695 -1 0.177752 7 -0.9207646 12 13 -1 -0.033601 -1 -0.00571418 111 1.605263 15"
   "28 1 -1.86478 16 17 -1 -0.069651 28 0.5679438 18 21 139 -0.9264476 19 20 -1 0.0807719 -1 -0.0532404 9 0.6716524 22 27 198 0.3499997 23 26 112 0.340909 24 25 -1 0.140934 -1 0.0400866 -1 0.0174744 -1 0.0155364 -1 -0.0187039 29 244 1.527777 1 28 233 -0.2639627 2 13 277 2.5 3 10 260 0.2020905 4 9 119 -1.130977 5 6 -1 0.049662 256 -0.3540439 7 8 -1 0.0193252 -1 -0.045742 -1 0.0255655 143 -0.4459323 11 12 -1 -0.0164372 -1 0.0255447 38 0.3220339 14 21 216 -0.6677778 15 20 30 0.6524276 16 19 215 -1.1675 17 18 -1 -0.0272137 -1 0.0806669 -1 -0.12659 -1 -0.0373107 95 -0.8550347 22 23 -1 0.0237063 26 0.5611015 24 25 -1 -0.0355711 7 -0.4519286 26 27 -1 -0.0392256 -1 0.00316459 -1 -0.0321857 29 218 2.968749 1 28 93 0.4526093 2 11 146 1.159894 3 8 188 0.7486411 4 5 -1 0.00158632 179 -1.446808 6 7 -1 0.0794118 -1 -0.027519 145 -0.869063 9 10 -1 0.14002 -1 -0.0154293 139 0.7976389 12 19 116 0.4638167 13 18"
   "178 1.071428 14 15 -1 -0.0231351 125 0.1263601 16 17 -1 -0.034154 -1 0.280992 -1 -0.0738184 209 -0.4560088 20 25 110 0.2499998 21 22 -1 -0.0561412 38 0.2264151 23 24 -1 -0.0259989 -1 0.192865 7 0.8934453 26 27 -1 -0.0359329 -1 0.0762227 -1 -0.0726822 29 63 -0.2926828 1 12 30 0.09759956 2 5 95 -0.6995791 3 4 -1 0.00210181 -1 0.0229987 178 -0.7499993 6 7 -1 -0.0624244 39 0.05555554 8 9 -1 -0.0347814 217 -0.2250883 10 11 -1 -0.0144286 -1 0.0376725 77 0.0389452 13 22 56 0.6339081 14 21 1 -4.096528 15 16 -1 -0.100829 258 -1.008874 17 18 -1 -0.0427126 143 -0.6023957 19 20 -1 -0.0107976 -1 0.0174826 -1 -0.0293288 71 -2.612923 23 24 -1 -0.0774767 108 -0.9189185 25 26 -1 0.121205 51 -0.1259547 27 28 -1 0.0164161 -1 -0.0198235 29 168 -7.415331 1 2 -1 -0.0820686 101 1.688481 3 28 79 -4.74587 4 9 255 0.7448613 5 6 -1 -0.0391846 210 0.3404887 7 8 -1 0.0349488 -1 -0.0667705 134 -2.812499 10 17 151 -0.1818181"
   "11 14 97 -0.06079233 12 13 -1 0.0389395 -1 0.174813 203 0.4230768 15 16 -1 -0.0645113 -1 0.15262 45 1.285714 18 25 282 7 19 22 163 -0.9656395 20 21 -1 0.0703797 -1 -0.031594 23 0.2719452 23 24 -1 0.0113737 -1 -0.00217258 97 0.3701494 26 27 -1 0.00688903 -1 0.067598 -1 0.0547076 29 114 2.249999 1 28 14 2.874999 2 27 62 -0.7142853 3 12 210 -1.702993 4 7 23 0.4071657 5 6 -1 -0.0702047 -1 0.0761321 39 0.3749999 8 11 206 1.127659 9 10 -1 -0.0467554 -1 0.09843 -1 0.00292031 255 0.2311756 13 26 156 -0.015625 14 17 122 0.02988932 15 16 -1 -0.00566497 -1 0.016691 256 -0.3859192 18 21 94 -0.4084288 19 20 -1 -0.0480688 -1 0.0193141 23 0.9209927 22 25 213 1.45394 23 24 -1 -0.0167206 -1 0.0834215 -1 0.0118331 -1 0.0103871 -1 -0.0777036 -1 0.0698499 29 26 0.01086693 1 14 167 -4.240697 2 3 -1 0.173754 199 -0.4392857 4 11 30 0.001521526 5 8 157 -0.4999998 6 7 -1 -0.075973 -1 0.0202106 281 108 9 10 -1 -0.0499237"
   "-1 0.0832422 282 115 12 13 -1 -0.0747241 -1 -0.00887544 25 0.0004911874 15 20 110 0.09999998 16 19 33 0.999999 17 18 -1 0.105248 -1 -0.0363104 -1 -0.0168303 19 2.571428 21 26 138 -1.032512 22 23 -1 -0.0137144 156 -1.05 24 25 -1 0.061249 -1 0.00110344 281 134 27 28 -1 -0.0668448 -1 0.032266 29 130 -0.7272726 1 4 194 0.7035022 2 3 -1 0.00275029 -1 0.0443743 281 146 5 24 138 -1.229235 6 7 -1 0.0325747 144 -1.57142 8 9 -1 -0.0656042 96 -0.1924491 10 11 -1 0.00976421 145 -0.03241167 12 15 107 -0.7222221 13 14 -1 0.00723371 -1 -0.034086 39 0 16 17 -1 -0.0340171 218 0.2840908 18 19 -1 0.000112766 30 0.012958 20 21 -1 -0.0423515 202 0.5955881 22 23 -1 0.0902593 -1 -0.0229766 140 -0.6181005 25 26 -1 -0.00491694 218 0.8382351 27 28 -1 -0.0708131 -1 0.123117 29 247 0.3124999 1 12 56 0.6627882 2 7 26 0.2350525 3 4 -1 0.00512516 113 0.06666663 5 6 -1 -0.0175288 -1 0.00342579 190 0.5580325 8 9 -1 -0.0237051"
   "255 0.6138734 10 11 -1 0.0635743 -1 -0.0599201 277 6.5 13 24 3 -0.1402249 14 21 156 0.01886792 15 16 -1 0.0331373 23 0.7228911 17 20 214 -0.01526758 18 19 -1 0.00990694 -1 -0.052766 -1 0.0387695 73 0.8095451 22 23 -1 -0.0212564 -1 0.0405622 164 -0.6313855 25 26 -1 -0.0272512 212 -0.3083002 27 28 -1 0.121195 -1 0.0363631 29 188 -2.269782 1 2 -1 -0.0775609 182 1.657894 3 28 138 0.6486447 4 17 95 0.4958109 5 14 210 0.3612511 6 7 -1 0.000905835 27 0.4427079 8 11 55 -0.6222042 9 10 -1 -0.0411691 -1 0.0747223 143 0.2607514 12 13 -1 -0.0419694 -1 0.0145572 138 -0.2586688 15 16 -1 -0.0263209 -1 0.0329418 282 60 18 21 17 0.3999997 19 20 -1 -0.0832978 -1 -0.00269233 28 0.5239866 22 25 205 0.4117646 23 24 -1 -0.00188833 -1 -0.0950945 118 0.9765914 26 27 -1 0.0108742 -1 -0.130996 -1 0.0479784 29 132 1.173077 1 28 133 0.8333322 2 3 -1 -0.000498697 120 -0.4476639 4 9 206 -0.4772726 5 6 -1 -0.043592 84"
   "0.5714284 7 8 -1 0.115269 -1 -0.0027263 178 0.9038459 10 19 200 1.874993 11 16 223 0.999998 12 13 -1 -0.0317881 132 0.3804347 14 15 -1 0.161617 -1 -0.0342808 184 0.3570627 17 18 -1 -0.00269581 -1 0.232781 209 0.006047234 20 21 -1 0.000225045 8 0.1852192 22 23 -1 0.0356512 22 0.183908 24 27 118 0.4816917 25 26 -1 0.616381 -1 -0.0234236 -1 -0.011402 -1 -0.0248608 29 19 0.6666664 1 10 42 0.8095237 2 3 -1 0.000822482 143 -0.8992937 4 7 229 0.1388888 5 6 -1 0.0800398 -1 -0.0314566 223 -0.8749965 8 9 -1 0.0745811 -1 -0.0388602 126 -0.3499999 11 12 -1 -0.0272813 195 0.4874998 13 28 24 0.4477632 14 25 194 -0.6391587 15 20 198 -0.5078124 16 17 -1 0.0369693 69 -1.220852 18 19 -1 0.0668618 -1 -0.0792845 25 0.02351897 21 24 183 -0.5370369 22 23 -1 -0.0342147 -1 0.0688921 -1 0.00699977 167 -0.08620636 26 27 -1 -0.00131452 -1 0.0846544 -1 -0.00897104 29 199 0.1648936 1 2 -1 -0.00178077 228 0.4473683 3"
   "28 119 0.08119897 4 15 8 0.5315175 5 14 39 1.499999 6 13 186 0.1376984 7 12 26 0.04217046 8 9 -1 0.150928 19 0.4285713 10 11 -1 -0.0367503 -1 0.0716544 -1 -0.0193003 -1 0.120354 -1 0.0672403 105 -0.2266666 16 21 97 0.2345914 17 20 34 0.2608695 18 19 -1 -0.0167133 -1 0.0947395 -1 -0.0354063 193 -0.1223601 22 23 -1 -0.0173589 41 0.3968253 24 25 -1 0.00170741 197 0.6312499 26 27 -1 0.0481095 -1 0.176628 -1 -0.0203565 29 196 1.428571 1 24 250 0.9687495 2 19 40 0.03125 3 18 177 -0.5999995 4 7 35 0.2173913 5 6 -1 -0.0338197 -1 0.0870041 132 -0.368421 8 11 17 -0.6153844 9 10 -1 -0.0246666 -1 -0.0898108 50 -0.3156951 12 15 203 -0.4166665 13 14 -1 0.0422997 -1 -0.0542906 202 0.7499985 16 17 -1 0.0284986 -1 -0.0641183 -1 0.00186553 159 0.3870967 20 21 -1 -0.0265081 112 -0.02803738 22 23 -1 -0.0373527 -1 0.0387081 212 -0.6751622 25 26 -1 0.118419 141 -0.8927635 27 28 -1 -0.100418 -1 0.0322578 29 100"
   "0.6084395 1 14 1 -0.05350758 2 5 231 0.08662909 3 4 -1 0.000399304 -1 -0.0200962 82 -0.8347825 6 11 157 -0.2916666 7 8 -1 -0.0238697 116 0.004317789 9 10 -1 0.0223912 -1 0.129972 281 114 12 13 -1 0.0112913 -1 -0.0103651 215 0.9853616 15 24 138 0.9517897 16 21 222 -1.055921 17 20 179 -0.2599999 18 19 -1 0.0850376 -1 -0.0402575 -1 -0.0401493 134 -0.2613636 22 23 -1 -0.0423814 -1 0.0734249 281 88 25 28 175 -0.7948716 26 27 -1 0.208281 -1 -0.0659783 -1 0.116278 29 218 -3.490384 1 10 168 -1.032868 2 7 171 -1.395021 3 6 96 0.5824224 4 5 -1 0.0675401 -1 -0.103481 -1 0.164722 105 -0.02777777 8 9 -1 0.0403505 -1 -0.0838023 38 0.2264151 11 12 -1 0.00198748 39 0.09999996 13 16 31 0.2860962 14 15 -1 0.0116169 -1 -0.0466138 120 0.2226504 17 26 222 0.6184208 18 25 281 80 19 20 -1 0.0199606 218 0.5833332 21 22 -1 -0.0298916 168 -0.09101836 23 24 -1 0.10027 -1 -0.0579839 -1 0.0709875 280 0.2386364 27 28"
   "-1 0.0456835 -1 -0.0395936 29 241 -1.216666 1 8 178 0.2142857 2 7 9 -1.410273 3 6 213 0.6015572 4 5 -1 0.0294143 -1 0.242021 -1 -0.00903528 -1 0.0415931 7 -7.946742 9 10 -1 -0.063711 158 0.2372881 11 26 55 -0.111778 12 25 71 -0.4862065 13 18 236 0.7160451 14 17 221 0.2187499 15 16 -1 -0.0166675 -1 0.0164929 -1 0.0422516 48 0.6018472 19 20 -1 -0.027773 220 -0.2302631 21 24 235 0.0448694 22 23 -1 -0.0211933 -1 0.170948 -1 -0.0207085 -1 0.0117763 197 -1.041666 27 28 -1 0.0307758 -1 -0.00943288 29 227 -0.6249975 1 20 122 0.9717686 2 19 198 0.6517855 3 18 200 437500 4 17 126 0.116279 5 6 -1 0.0203939 225 0.4791663 7 16 40 0.468085 8 11 47 0.05725811 9 10 -1 -0.057841 -1 0.0375143 196 -0.1874998 12 15 106 0.2916665 13 14 -1 -0.0227566 -1 -0.233584 -1 0.0314439 -1 -0.119134 -1 0.153418 -1 0.0596105 -1 -0.0451415 223 -1.166666 21 22 -1 -0.0742884 222 -6.642853 23 24 -1 -0.101603 172 -4.464283 25"
   "26 -1 0.0616439 281 90 27 28 -1 -0.00659942 -1 0.00422269 29 8 -2.310031 1 2 -1 -0.0601159 113 -1.083332 3 10 30 0.02698445 4 7 211 -1.926561 5 6 -1 0.153413 -1 -0.0430649 118 0.1618685 8 9 -1 0.0367907 -1 -0.0556867 20 -0.9999996 11 14 175 -1.583332 12 13 -1 0.0731087 -1 -0.0319647 202 -0.8749996 15 26 223 1.05 16 23 14 -0.6249997 17 18 -1 0.0659641 184 -0.3691988 19 22 282 15 20 21 -1 0.0245639 -1 -0.0486452 -1 0.0924278 99 -2.155918e-05 24 25 -1 -0.0243721 -1 0.20138 22 -1.272727 27 28 -1 -0.0447175 -1 -6.51033e-05 29 183 -0.4999998 1 20 186 0.05977825 2 13 177 -0.02777777 3 6 143 -1.252353 4 5 -1 -0.0802913 -1 0.00563818 169 0.5010511 7 12 180 -1.026315 8 11 42 0.1136364 9 10 -1 0.0382362 -1 -0.113053 -1 -0.033263 -1 0.0490123 173 0.07812498 14 17 29 0.2829482 15 16 -1 0.0587623 -1 -0.00975688 198 0.1874998 18 19 -1 0.143845 -1 -0.0174099 86 -0.3333331 21 28 72 -0.6588306 22 23 -1 0.00276994"
   "49 -0.514759 24 25 -1 0.0586227 132 -0.4687499 26 27 -1 -0.0321783 -1 0.0311214 -1 -0.00400965 29 248 -0.999996 1 20 265 0.8571424 2 19 172 0.8333322 3 18 172 -2.149999 4 5 -1 0.123006 71 -0.7020946 6 7 -1 -0.0403448 220 0.3194443 8 9 -1 -0.00483573 200 -0.1666666 10 11 -1 -0.0664766 259 -0.4666629 12 13 -1 -0.0512597 180 -0.2499999 14 15 -1 -0.0351359 12 0.4999998 16 17 -1 0.229086 -1 0.0231558 -1 0.086261 -1 0.0792486 110 0.2499995 21 22 -1 0.00186411 153 0.09042552 23 24 -1 -0.019662 42 0.3305785 25 28 3 0.2347115 26 27 -1 0.076818 -1 -0.0434856 -1 -0.00487017 29 194 -2.057841 1 18 126 0.0140845 2 13 224 0.6363632 3 12 204 0.5421686 4 9 201 0.7187493 5 8 131 0.06249998 6 7 -1 -0.0814654 -1 -0.0118713 -1 0.124528 110 -0.125 10 11 -1 -0.0486833 -1 0.0898192 -1 -0.131757 201 -0.1499999 14 15 -1 0.0415611 102 -0.01539575 16 17 -1 0.0144309 -1 -0.0803431 221 1.014705 19 20 -1 -5.83202e-05 151"
   "-0.07812498 21 24 134 0.07586206 22 23 -1 -0.0503521 -1 0.0439392 3 -0.8961077 25 28 140 -0.930192 26 27 -1 -0.0602125 -1 0.162513 -1 0.0347828 29 84 0.4673913 1 14 117 0.4089143 2 3 -1 -4.40531e-05 24 0.7167916 4 7 222 0.1721311 5 6 -1 0.019667 -1 -0.0436207 28 0.5330184 8 9 -1 0.0999145 151 -0.1111111 10 11 -1 -0.0412338 279 15.5 12 13 -1 -0.00594117 -1 0.0929532 11 -0.2999999 15 22 139 -0.3562549 16 19 187 -1.144095 17 18 -1 0.0680341 -1 -0.0351561 174 0.3026315 20 21 -1 0.0469414 -1 -0.0386142 23 0.4683957 23 24 -1 0.024891 214 0.8721749 25 26 -1 -0.0295571 218 0.2173912 27 28 -1 -0.028731 -1 0.0785256 29 163 -0.6717363 1 8 118 -0.7040723 2 3 -1 0.0006425 161 -1.161113 4 5 -1 -0.0689583 181 0.9428569 6 7 -1 0.0457867 -1 -0.0163166 281 145 9 20 222 -0.4062499 10 11 -1 -0.0262798 222 0.002717391 12 17 206 0.833333 13 16 223 1.281249 14 15 -1 0.0132898 -1 0.0711149 -1 -0.0541176 107 -1.931817"
   "18 19 -1 0.039294 -1 -0.00688098 93 -2.456876 21 24 167 -0.7499963 22 23 -1 -0.086964 -1 0.115048 224 -0.2499998 25 28 279 20.5 26 27 -1 -0.00664097 -1 0.139042 -1 -0.0465523 29 179 -0.5441175 1 22 197 -0.1388889 2 13 224 -0.7499993 3 12 127 -0.4999998 4 5 -1 -0.0260969 201 -0.7499998 6 7 -1 -0.0460014 175 -0.8333328 8 9 -1 -0.00415503 23 0.6707923 10 11 -1 0.225469 -1 -0.0469026 -1 -0.00746615 69 0.396604 14 17 190 0.1715677 15 16 -1 -0.0504234 -1 -0.00759858 13 0.6279069 18 19 -1 -0.00118997 69 0.6282522 20 21 -1 0.323587 -1 0.0413917 217 0.7932674 23 24 -1 0.00130993 222 0.1971154 25 26 -1 0.00508551 40 0.6249997 27 28 -1 0.0495191 -1 0.267656 29 244 0.4166664 1 26 37 0.5999998 2 19 119 0.05689061 3 16 224 0.580357 4 15 195 0.0338983 5 8 164 0.7921373 6 7 -1 0.015648 -1 -0.152934 56 0.4778219 9 14 170 -0.5056172 10 13 219 -0.6060603 11 12 -1 0.00190187 -1 -0.0573851 -1 -0.000938411 -1"
   "0.0177283 -1 -0.0245328 164 -0.2020262 17 18 -1 -0.0377556 -1 -0.00525366 201 0.6545453 20 23 229 -1.522727 21 22 -1 -0.0551842 -1 0.016572 215 -0.1104447 24 25 -1 -0.0523376 -1 0.0225047 72 0.3313882 27 28 -1 -0.0140687 -1 0.0241195 29 147 0.1078887 1 14 168 -1.00189 2 5 110 -0.6666662 3 4 -1 0.0227839 -1 -0.0305702 137 -0.3249999 6 7 -1 -0.00408278 145 -0.9375796 8 9 -1 -0.0290459 130 0.1104651 10 13 38 0.07999999 11 12 -1 -0.0114095 -1 0.0282642 -1 -0.000573314 168 -1.256385 15 28 190 0.6064779 16 21 49 -0.1653664 17 20 221 0.2749998 18 19 -1 -0.00275477 -1 0.0891942 -1 -0.0480021 217 -0.2994631 22 27 2 -1.383871 23 24 -1 0.0507172 128 0.01041666 25 26 -1 0.316464 -1 0.037517 -1 -0.0188272 -1 -0.00912055 29 42 0 1 14 10 -1.319005 2 13 116 -1.356334 3 4 -1 0.070595 64 -0.499999 5 12 62 -0.7142853 6 7 -1 0.0771841 84 -1.038461 8 11 201 -0.2604166 9 10 -1 0.00394396 -1 -0.201007 -1 -0.0237538"
   "-1 0.0350666 -1 -0.0493712 10 1.11003 15 24 36 0.2857143 16 17 -1 0.00720818 151 -0.9024389 18 21 171 0.6756605 19 20 -1 -0.0301535 -1 0.0905946 251 0.5937497 22 23 -1 -0.0050494 -1 0.0136935 281 162 25 28 161 0.06144055 26 27 -1 0.0585718 -1 -0.0146438 -1 -0.0854462 29 213 -0.559025 1 10 273 -0.6744184 2 9 13 0.2068965 3 6 247 -0.645161 4 5 -1 0.0933763 -1 -0.0354651 183 -0.4565216 7 8 -1 0.142881 -1 0.0207426 -1 -0.0199736 281 26 11 24 227 -0.2999998 12 13 -1 -0.0290334 159 -0.3673468 14 15 -1 0.110446 134 -0.1935483 16 17 -1 -0.0202364 127 0.6562498 18 23 93 0.6967549 19 20 -1 0.0499595 190 -0.09895776 21 22 -1 -0.0498949 -1 0.28612 -1 -0.0485025 212 -0.8068659 25 28 177 -0.4374999 26 27 -1 0.120117 -1 0.011481 -1 -0.000517702 29 282 196 1 16 41 0.04545454 2 15 248 -0.02678571 3 6 126 0.06521738 4 5 -1 0.0151928 -1 -0.0197495 281 177 7 10 131 -0.4166664 8 9 -1 -0.00140956 -1 -0.0789353"
   "26 0.9358237 11 14 79 0.09287626 12 13 -1 0.0454131 -1 -0.0607321 -1 0.0685072 -1 0.000958859 204 2.749996 17 28 206 0.4545454 18 27 109 0.08333322 19 24 154 0.1071428 20 23 156 -3.469447e-18 21 22 -1 -0.0106508 -1 -0.101789 -1 0.0314859 282 233 25 26 -1 0.0508221 -1 -0.0458434 -1 0.0608136 -1 -0.118923 29 268 -0.0833333 1 6 240 0.009969269 2 5 172 -3.999995 3 4 -1 -0.0624474 -1 0.00531904 -1 -0.0143617 254 0.04663396 7 28 197 -0.4758064 8 23 79 0.4837925 9 18 76 -0.3177861 10 11 -1 -0.0479158 162 -0.4706135 12 13 -1 0.0547887 104 -0.8636362 14 17 134 0.6153844 15 16 -1 -0.111797 -1 0.0840215 -1 -0.00848527 200 0 19 22 206 -0.9374994 20 21 -1 -0.017401 -1 0.0679266 -1 -0.032854 195 0.3461537 24 27 26 0.9533039 25 26 -1 0.0223464 -1 -0.00654869 -1 -0.00309991 -1 -0.00595004 29 195 -5.425923 1 2 -1 0.0580246 199 -4.349998 3 4 -1 -0.0609851 186 -1.351333 5 8 201 -1.499999 6 7 -1 -0.0864158 -1"
   "0.0332211 187 -1.26564 9 16 225 0.04545453 10 15 109 -0.5555554 11 14 194 -19.83188 12 13 -1 -0.125033 -1 0.0481225 -1 -0.0649296 -1 -0.0632024 196 -1.145833 17 20 15 -0.7631578 18 19 -1 0.077178 -1 -0.0199741 151 -0.9999998 21 24 131 -0.2499998 22 23 -1 0.00743183 -1 -0.0393987 203 -1.210526 25 28 109 -0.9642854 26 27 -1 0.0909558 -1 -0.0485137 -1 0.0025785 29 168 1.138015 1 28 176 0.2558139 2 3 -1 -0.00133185 159 0.3214285 4 7 186 1.156168 5 6 -1 -0.00118028 -1 0.0868699 153 0.7499998 8 27 25 0.8568883 9 16 144 -0.3947356 10 13 178 0.8035712 11 12 -1 -0.0071513 -1 -0.202461 233 0.07682233 14 15 -1 0.0515887 -1 -0.0155456 157 0.323741 17 26 113 0.8484844 18 23 151 0.3166666 19 22 19 0.1333333 20 21 -1 -0.038587 -1 0.236937 -1 -0.0641418 28 0.7579996 24 25 -1 -0.0243064 -1 0.386946 -1 -0.0248103 -1 -0.0676624 -1 -0.0420061 29 31 0.1874086 1 18 281 226 2 17 36 0.391705 3 14 229 -0.3416666"
   "4 9 36 0.02702702 5 6 -1 -0.0467486 103 0.3214285 7 8 -1 0.0515803 -1 -0.0439721 41 0.499999 10 11 -1 -0.0676638 96 -0.5545448 12 13 -1 -0.0536115 -1 0.0358541 157 1.291666 15 16 -1 -0.0902911 -1 0.0892211 -1 0.0733013 18 -0.999996 19 28 186 0.4906634 20 27 18 -0.9999992 21 26 220 0.3663793 22 25 2 0.1795626 23 24 -1 0.0515763 -1 -0.0458934 -1 -0.0473949 -1 0.0777244 -1 -0.0535431 -1 -0.000164024 29 218 2.968749 1 28 218 1.660713 2 23 132 2.285714 3 22 33 1.040759 4 21 170 0.5150514 5 6 -1 -0.00122385 203 0.01562499 7 12 25 0.08816359 8 11 214 -0.9526598 9 10 -1 0.0586496 -1 -0.0709665 -1 0.00779365 179 0.03448274 13 16 177 -0.6999999 14 15 -1 -0.0691614 -1 0.0650033 191 0.150724 17 18 -1 -0.0205194 168 -0.1857455 19 20 -1 0.156426 -1 0.00798821 -1 -0.0493625 -1 -0.0647143 91 -0.2666666 24 25 -1 -0.019748 150 -0.3888887 26 27 -1 0.203202 -1 0.040272 -1 -0.0714307 29 48 0.253984 1 24 255 -1.191135"
   "2 3 -1 0.0902863 182 0.225 4 15 106 0.3499997 5 6 -1 0.000494081 156 -0.8888873 7 8 -1 0.0836337 28 0.932647 9 14 176 0.8305084 10 13 162 -3.260171e-05 11 12 -1 -0.00375914 -1 -0.0454154 -1 0.113847 -1 -0.058829 175 -0.9333331 16 17 -1 -0.0584032 280 0.1914894 18 23 134 0.2 19 22 2 -0.5085793 20 21 -1 0.0356373 -1 -0.0415081 -1 0.0584548 -1 0.00433132 250 -0.6136361 25 26 -1 0.0305657 97 -1.729551 27 28 -1 0.0823163 -1 -0.0377319 29 177 2.749996 1 22 271 1.499997 2 21 133 0.7999994 3 16 129 0.9583327 4 11 116 0.4179668 5 6 -1 -0.00134558 201 -0.006578946 7 8 -1 -0.00520595 281 153 9 10 -1 0.0338454 -1 0.196281 121 0.1586197 12 15 80 -0.9999995 13 14 -1 0.119337 -1 -0.036748 -1 0.0441959 130 0.2211055 17 18 -1 -0.0368264 24 0.8149729 19 20 -1 0.0426093 -1 -0.0531333 -1 0.0320441 205 -0.6818179 23 26 201 -0.7678567 24 25 -1 -0.0558571 -1 0.0723884 148 0.06898109 27 28 -1 -0.0604755 -1 0.00831466"
   "29 56 -0.3479526 1 4 36 0.4333333 2 3 -1 -0.0283551 -1 0.00109079 192 1.086369 5 26 231 -0.582323 6 9 125 0.1263601 7 8 -1 0.0340668 -1 -0.00803405 26 0.4763935 10 25 198 0.3181817 11 22 209 1.002587 12 21 43 0 13 20 62 -0.3333332 14 19 92 -0.8975713 15 16 -1 -0.0586814 36 0.7798164 17 18 -1 0.241901 -1 -0.0159356 -1 -0.0435086 -1 -0.0433107 -1 0.206202 2 -0.3066753 23 24 -1 0.040419 -1 -0.0585289 -1 0.00289317 113 -0.02631578 27 28 -1 0.0374582 -1 -0.113068 29 6 -0.2272711 1 2 -1 0.00394897 119 -0.604122 3 8 147 -0.01213819 4 7 280 0.9323308 5 6 -1 -0.0216322 -1 0.0307667 -1 -0.047899 111 0.6315787 9 28 152 -0.9999996 10 11 -1 0.0552313 111 -1.394736 12 13 -1 -0.0529836 101 -0.4018309 14 15 -1 -0.0169726 144 -0.3669713 16 17 -1 0.0327314 281 213 18 25 117 -0.9558506 19 24 131 0.2999998 20 21 -1 0.00517675 215 0.3357427 22 23 -1 0.0136728 -1 0.128217 -1 -0.00459354 225 0.2105262 26 27 -1"
   "0.159396 -1 -0.0279967 -1 -0.0186979 29 146 0.4958492 1 6 151 -0.2268041 2 3 -1 0.00406528 191 -0.0200387 4 5 -1 -0.0175037 -1 0.00316462 188 -0.2088362 7 8 -1 0.0954033 110 0.04999996 9 24 151 0.1538461 10 17 133 -0.1388888 11 16 161 0.7333765 12 15 111 -0.2 13 14 -1 0.0557355 -1 -0.0122907 -1 -0.0952832 -1 -0.0425803 204 0.9749996 18 21 0 -0.5202954 19 20 -1 0.090635 -1 0.0209435 251 -0.5744679 22 23 -1 0.0648343 -1 -0.112271 223 -0.7575756 25 26 -1 0.114392 282 209 27 28 -1 -0.0208411 -1 0.126565 29 243 1.277777 1 28 282 77 2 7 144 -0.6082439 3 6 26 0.005507979 4 5 -1 0.034066 -1 -0.0201483 -1 0.00707652 281 84 8 15 154 -0.2499998 9 14 30 0.001521526 10 11 -1 -0.0305679 229 -0.1527777 12 13 -1 -0.0129639 -1 0.0558856 -1 -0.00588413 19 -1.749999 16 19 23 0.6580529 17 18 -1 -6.9825e-05 -1 0.096233 193 -0.951258 20 21 -1 0.0750809 196 0.8333322 22 23 -1 -0.0405801 82 0.3934425 24 25 -1 -0.028361"
   "58 1.199999 26 27 -1 0.157516 -1 -0.0551023 -1 0.0207062 29 21 0.2777777 1 2 -1 0.00250786 151 -0.2830188 3 4 -1 -0.0257018 156 0.2291666 5 24 218 0.5714284 6 23 187 0.01888076 7 12 223 0.2249999 8 9 -1 -0.0237462 122 -0.0003014618 10 11 -1 -0.00673272 -1 0.0510466 281 195 13 22 281 115 14 21 280 0.6350711 15 20 93 0.3066017 16 17 -1 -0.0444946 126 0.1785713 18 19 -1 -0.21556 -1 -0.0365434 -1 -0.0175718 -1 0.0247458 -1 -0.137679 -1 0.0265821 138 0.5054971 25 28 54 -0.09629537 26 27 -1 -0.00971014 -1 0.0307181 -1 -0.0150724 29 212 0.3552927 1 10 215 0.3472246 2 5 118 -2.239266 3 4 -1 -0.030103 -1 0.00206011 1 0.2113496 6 7 -1 0.0214806 205 0.225 8 9 -1 0.0459311 -1 0.294263 202 0.01086956 11 16 105 -1.652777 12 15 3 -1.028486 13 14 -1 0.185941 -1 -0.0208664 -1 -0.0286768 141 0.0260822 17 28 25 0.2604601 18 19 -1 -0.0052761 156 -0.3571427 20 21 -1 -0.0514603 207 0.4525558 22 23 -1 0.0277496"
   "224 0.3404254 24 27 191 -1.105561 25 26 -1 -0.0651683 -1 0.162102 -1 0.0364484 -1 -0.0147782 29 44 0.1204819 1 14 31 0.2668284 2 11 61 -0.2222222 3 8 33 0.9794736 4 5 -1 0.0130112 218 0.1666666 6 7 -1 -0.0523161 -1 0.0240172 268 0.04032258 9 10 -1 0.0757443 -1 -0.0365921 43 0.06369425 12 13 -1 -0.0962366 -1 -0.0246857 229 -2.454545 15 16 -1 0.103076 36 0.2222222 17 18 -1 0.0111012 19 0.6249997 19 28 42 0.7818181 20 27 227 0.1507936 21 26 282 105 22 23 -1 -0.0209697 156 -0.772727 24 25 -1 -0.0803833 -1 0.0225391 -1 0.00652927 -1 -0.0284532 -1 0.0114223 29 272 187500 1 28 11 1.666664 2 15 175 -1.999998 3 4 -1 -0.0734245 131 0 5 6 -1 0.00434425 36 0.3636363 7 8 -1 0.00723448 21 -0.07142855 9 10 -1 -0.0278117 281 10 11 14 151 0 12 13 -1 0.26399 -1 0.00633617 -1 -0.00585978 102 0.6232508 16 27 111 1.605263 17 26 3 -3.172377 18 19 -1 -0.0761544 177 0.6249994 20 23 42 0.911111 21 22 -1 0.0728829"
   "-1 0.00911445 57 1.8 24 25 -1 0.0495841 -1 0.173285 -1 -0.012278 -1 -0.0621521 -1 -0.0716278 29 49 0.2466304 1 22 141 0.150343 2 13 164 0.3743505 3 12 231 -0.0270429 4 7 278 2.5 5 6 -1 -0.0138715 -1 0.0161645 38 0.2962963 8 9 -1 -0.0344198 256 -0.02450237 10 11 -1 -0.0120878 -1 0.00857621 -1 -0.0298714 223 5.374979 14 21 70 0.1286074 15 16 -1 -0.0256833 34 0.7835051 17 20 221 0.15 18 19 -1 -0.0514297 -1 0.0134283 -1 0.0406155 -1 0.0953229 213 -0.009259191 23 24 -1 0.0231759 191 0.8535774 25 26 -1 -0.000131997 260 0.1331277 27 28 -1 -0.0881837 -1 0.0771561 29 188 0.7486411 1 6 184 0.5591784 2 3 -1 -0.000260352 197 0.899999 4 5 -1 0.0107016 -1 0.101722 201 0.2499999 7 22 222 0.1305555 8 15 170 0.2526868 9 14 134 0.5760869 10 11 -1 -0.0600201 203 0.2980768 12 13 -1 0.105098 -1 -0.0643704 -1 0.0199084 36 0.8666666 16 19 246 -0.8249998 17 18 -1 0.100156 -1 -0.034506 70 0.09732974 20 21 -1 0.134222"
   "-1 -0.050389 178 -0.3076922 23 28 110 0.4166661 24 27 163 -3.090479 25 26 -1 0.103464 -1 -0.0558879 -1 0.112169 -1 -0.0680632 29 112 0.3333329 1 8 168 -2.775894 2 7 201 0.4218748 3 4 -1 0.0526965 188 0.3218781 5 6 -1 0.0416211 -1 -0.129321 -1 -0.00401572 24 0.0007453508 9 10 -1 0.0677104 148 0.3691899 11 24 282 18 12 15 227 -0.125 13 14 -1 -0.0309818 -1 0.102564 192 -0.3374628 16 17 -1 -0.0255893 224 -0.6666665 18 19 -1 -0.0378972 245 -0.2115384 20 21 -1 -0.00869235 281 113 22 23 -1 0.00921561 -1 0.0499122 200 0.1041666 25 28 44 0.5806451 26 27 -1 0.0591516 -1 -0.0314238 -1 0.0817318 29 175 0.4342104 1 28 213 0.3571408 2 7 172 0.08510637 3 6 171 -0.554743 4 5 -1 -0.00804993 -1 0.0172181 -1 -0.00710503 174 1.076923 8 27 96 1.101547 9 26 189 -0.8619506 10 23 25 0.8908661 11 20 118 -0.9123331 12 13 -1 0.0530746 228 0.4666665 14 19 196 -0.4374996 15 16 -1 -0.0320896 96 -0.3242399 17 18 -1 -0.0258871"
   "-1 0.145838 -1 -0.0893552 34 0.7894736 21 22 -1 0.238189 -1 -0.0415062 222 0.6906474 24 25 -1 -0.0286742 -1 0.0723587 -1 0.0493222 -1 0.0524269 -1 0.00655804 29 136 1.657894 1 26 36 1.25 2 23 180 562500 3 22 17 1.333332 4 11 156 -1.142856 5 8 148 0.9793908 6 7 -1 -0.056502 -1 0.095301 233 0.6698443 9 10 -1 0.000173416 -1 0.0234779 233 -0.5511629 12 13 -1 0.05325 109 1.249999 14 21 87 -0.3333329 15 16 -1 -0.101004 220 0 17 20 206 0.2083332 18 19 -1 -0.0219685 -1 -0.111686 -1 0.000636132 -1 0.024927 -1 0.0713168 134 -0.2187499 24 25 -1 0.0337727 -1 -0.0612951 215 0.6477302 27 28 -1 0.0677865 -1 -0.0609159 29 260 -0.02303914 1 2 -1 -0.0043201 116 0.540144 3 28 125 0.1555026 4 21 108 62500 5 20 2 0.5464127 6 17 209 0.1586736 7 8 -1 -0.00578381 217 -0.9889534 9 10 -1 0.118294 61 -0.8749999 11 12 -1 -0.0507264 39 0 13 14 -1 0.0697661 36 0.03968253 15 16 -1 0.103624 -1 -0.00572816 142 0.04688823"
   "18 19 -1 -0.0671282 -1 0.0469987 -1 0.0787379 223 1.399999 22 27 227 0.7638885 23 24 -1 0.0106767 167 0.4693857 25 26 -1 0.0768346 -1 -0.0332134 -1 -0.0374967 -1 -0.0455203 29 136 -1.338983 1 8 132 -0.7142853 2 7 165 0.1018658 3 6 116 -0.852149 4 5 -1 -0.0264924 -1 0.0589631 -1 0.0686579 -1 -0.0490732 229 -2.454545 9 10 -1 0.100716 52 -1.941163 11 12 -1 0.0307976 44 0.0696864 13 24 31 0.2668284 14 19 158 -0.2499999 15 18 93 -0.2693996 16 17 -1 0.00707458 -1 0.123741 -1 -0.0312449 126 0.7124999 20 23 111 -0.4166665 21 22 -1 -0.00769748 -1 -0.0845546 -1 0.0226475 260 -0.007325573 25 28 137 1.089285 26 27 -1 0.0122111 -1 -0.0448375 -1 -0.00196375 29 166 1.079635 1 24 28 1.048378 2 15 258 0.03925923 3 4 -1 -0.0060638 276 2.5 5 12 235 0.1674194 6 9 279 28.5 7 8 -1 0.00699495 -1 -0.027392 117 -0.1623332 10 11 -1 0.0396737 -1 -0.00298611 248 -0.6428568 13 14 -1 0.0360211 -1 -0.014014 183 -2.775558e-17"
   "16 19 72 -0.008283384 17 18 -1 0.0760903 -1 -0.00931021 29 0.7615375 20 21 -1 0.0379608 163 0.001764128 22 23 -1 -0.0606413 -1 0.0297858 176 0.09999998 25 28 98 0.6999961 26 27 -1 -0.0545641 -1 0.0194224 -1 0.0718169 29 195 0.6363634 1 14 264 -1.95238 2 3 -1 0.0264363 42 1.018518 4 13 218 2.968749 5 12 117 1.396029 6 11 196 -0.6160713 7 10 45 0.117647 8 9 -1 -0.0379728 -1 0.023667 -1 -0.00227158 -1 -0.0586051 -1 0.099639 -1 -0.0194328 214 -0.1582603 15 26 171 0.4361556 16 23 158 -0.2749999 17 22 36 0.2647059 18 19 -1 0.0692731 116 0.06193617 20 21 -1 -0.0743219 -1 0.0718917 -1 0.0377835 28 0.2904891 24 25 -1 -0.0784567 -1 0.0944015 72 0.1978183 27 28 -1 -0.0158747 -1 0.0403598 29 199 1.512499 1 26 193 0.1303483 2 25 142 0.1919091 3 24 4 0.265764 4 23 184 0.03609023 5 12 196 0.01562499 6 11 139 0.09605064 7 10 198 0.1029411 8 9 -1 0.00158715 -1 0.0333519 -1 -0.037905 -1 -0.0173545 196 0.07499994"
   "13 18 223 0.3333329 14 15 -1 -0.03199 33 0.9971254 16 17 -1 -0.00529607 -1 0.0919741 149 0.2499998 19 22 265 0.5526315 20 21 -1 0.0622457 -1 -0.0298811 -1 0.00642274 -1 0.0262869 -1 -0.0168549 -1 -0.00529608 132 -0.3157892 27 28 -1 0.0180101 -1 -0.0863681 29 40 0.3749999 1 6 112 0.1346153 2 3 -1 -0.00206244 210 -3.169488 4 5 -1 -0.0906251 -1 0.0121572 221 0.15 7 12 255 -0.01137293 8 11 60 -0.6666664 9 10 -1 0.0586317 -1 -0.00533843 -1 -0.02869 120 0.1370368 13 22 189 -0.4038305 14 15 -1 -0.0171265 111 -0.6249998 16 19 110 -0.2999998 17 18 -1 -0.0119898 -1 0.0971549 263 -1.214052 20 21 -1 -0.047058 -1 0.0289316 122 -0.9433638 23 24 -1 -0.0637136 38 0.2713178 25 28 135 0.4374999 26 27 -1 -0.0168144 -1 0.0471991 -1 -0.0469115 29 264 -0.9549547 1 2 -1 0.0122871 34 0.8749999 3 12 177 0.3863636 4 11 218 0.3392855 5 8 218 -0.4044117 6 7 -1 -0.00613175 -1 0.0152797 76 -0.7293825 9 10 -1 0.0134129"
   "-1 -0.0268112 -1 -0.00693563 210 0.1165323 13 20 27 0.4257156 14 15 -1 -0.0703843 97 0.2345914 16 19 177 1.749999 17 18 -1 -0.00867525 -1 0.0985483 -1 -0.0450669 155 -0.499999 21 24 93 -0.6442272 22 23 -1 0.0121312 -1 -0.0941583 35 0.04545454 25 28 55 -0.403424 26 27 -1 -0.0269081 -1 0.1807 -1 0.0107947 29 200 218750 1 26 79 0.8066242 2 21 10 0.3862917 3 12 202 -1.124999 4 9 223 0.8124984 5 8 133 -0.3541665 6 7 -1 -0.0997256 -1 0.0186193 -1 -0.198722 10 0.3482405 10 11 -1 0.000140131 -1 -0.030074 117 0.2012861 13 20 33 0.9938058 14 17 223 0.8437492 15 16 -1 -0.0183977 -1 0.0609924 155 -0.7499997 18 19 -1 -0.0296265 -1 0.0399348 -1 -0.00470081 151 0.5312499 22 25 49 0.0758393 23 24 -1 -0.0432073 -1 0.00837488 -1 0.0301183 280 0.4625 27 28 -1 -0.0465867 -1 0.0809987 29 6 1.947354 1 26 206 2.01282 2 23 198 0.8902436 3 14 130 0.4499999 4 5 -1 0.00227588 122 0.7115219 6 9 200 0.1041666 7 8 -1"
   "-0.00319655 -1 -0.0468606 121 0.6363602 10 13 173 0.04999996 11 12 -1 0.0171176 -1 -0.0334582 -1 0.131832 11 0.2857141 15 22 122 0.3539895 16 21 154 -0.5999999 17 20 115 0.2538765 18 19 -1 -0.000381864 -1 0.223517 -1 -0.0334951 -1 0.0633891 -1 -0.0606782 210 0.2519978 24 25 -1 -0.000295364 -1 0.161975 175 -1.166666 27 28 -1 0.0954863 -1 -0.0635035 29 241 -0.2499998 1 28 141 -1.139664 2 7 165 0.2968733 3 6 122 -2.706498 4 5 -1 0.0157113 -1 -0.0693217 -1 0.00730664 277 4.5 8 25 130 -0.7272726 9 20 47 0.01832753 10 19 157 0.3749999 11 18 38 0.01923077 12 17 266 -0.2741935 13 16 91 -0.3636362 14 15 -1 -0.0981348 -1 0.0394547 -1 0.0477415 -1 -0.0166166 -1 0.0421182 -1 0.068992 143 0.6179914 21 24 176 0.235849 22 23 -1 -0.0288286 -1 0.0147121 -1 0.0354971 232 -0.003228323 26 27 -1 -0.0300816 -1 0.0130172 -1 0.00302446 29 90 -0.3333332 1 6 169 0.752572 2 5 26 0.747613 3 4 -1 0.00125719 -1 -0.0300831"
   "-1 -0.0447676 7 -0.9207646 7 8 -1 -0.00794703 7 -0.3722972 9 24 153 -0.916201 10 13 25 0.1156175 11 12 -1 0.0783191 -1 0.000176788 130 -0.7272726 14 23 220 0.9895832 15 22 220 0.3309859 16 21 176 -0.717647 17 20 38 0.3818182 18 19 -1 -0.0446019 -1 0.0630605 -1 0.0303915 -1 -0.0558341 -1 0.0674349 -1 0.0228207 108 2.249991 25 28 125 0.2867451 26 27 -1 0.000903421 -1 -0.0301983 -1 0.0695119 29 244 -0.9999993 1 2 -1 -0.0178096 160 2.249998 3 28 16 -0.6666664 4 13 263 -1.010639 5 8 103 -0.3593749 6 7 -1 -0.0571071 -1 0.0516939 210 -4.126218 9 10 -1 0.0852341 260 1.52757 11 12 -1 -0.0407569 -1 0.0323613 52 0.8571389 14 25 35 1.499999 15 22 196 1.026785 16 17 -1 0.00235451 213 -0.3031897 18 19 -1 0.0777203 69 0.1681462 20 21 -1 -0.0240714 -1 0.0519597 1 -0.01195671 23 24 -1 0.00922322 -1 -0.0885792 281 108 26 27 -1 -0.0228205 -1 0.0127495 -1 0.0797639 29 280 0.9481132 1 20 170 -0.2092097 2 5 263"
   "-0.03243909 3 4 -1 -0.0183741 -1 0.00110827 173 -0.09374998 6 7 -1 0.0155333 168 0.3673387 8 19 209 0.174225 9 12 104 0.1562499 10 11 -1 0.00549194 -1 -0.0173787 96 -0.3653809 13 14 -1 -0.0242833 180 0.7999994 15 18 203 1.910713 16 17 -1 0.0273182 -1 0.264112 -1 0.124865 -1 -0.0288768 85 -0.1666666 21 26 252 -0.2083333 22 23 -1 -0.0439873 169 -0.469869 24 25 -1 0.0250934 -1 -0.0427221 198 -0.7372879 27 28 -1 -0.0944889 -1 0.0382299 29 200 93750 1 26 207 -3.712738 2 3 -1 0.0863249 153 -4.694442 4 7 98 -1.244644 5 6 -1 0.0284279 -1 -0.0776695 140 -6.682759 8 9 -1 0.099109 3 -8.554197 10 11 -1 -0.0656656 92 -1.959921 12 15 190 -2.653449 13 14 -1 -0.0777913 -1 0.043652 208 -1.80439 16 21 25 0.06578656 17 20 24 0.01217762 18 19 -1 0.0747051 -1 -0.122659 -1 0.00892018 214 -1.00289 22 25 30 0.1624417 23 24 -1 0.0424968 -1 -0.0311246 -1 -0.000495064 19 -1.192982 27 28 -1 0.0232716 -1 -0.06532 29"
   "268 -0.06944443 1 14 282 93 2 13 42 0.9148935 3 8 124 0.6728358 4 7 38 0.8823529 5 6 -1 0.00808861 -1 -0.0312419 -1 0.0522193 144 0.5887063 9 10 -1 0.020442 28 0.4824618 11 12 -1 0.183655 -1 0.0453449 -1 -0.011085 30 0.06591861 15 16 -1 0.00252349 156 -0.8888873 17 18 -1 -0.0614458 264 -0.9549547 19 20 -1 0.0281041 24 0.0689278 21 26 1 -0.9366861 22 23 -1 -0.0704093 278 7.5 24 25 -1 -0.00334767 -1 0.155025 27 0.3306839 27 28 -1 -0.0551431 -1 -0.0101668 29 188 -2.269782 1 2 -1 -0.0723487 121 1.947354 3 28 98 1.008059 4 19 103 -1.472222 5 12 187 0.3307201 6 9 237 0.3772983 7 8 -1 -0.0751719 -1 0.0131249 219 0.1607142 10 11 -1 -0.0361743 -1 0.226048 203 -1.299999 13 16 26 0.2461301 14 15 -1 -0.069334 -1 0.0415287 194 -2.325447 17 18 -1 -0.0286408 -1 0.00116889 162 -1.448341 20 21 -1 0.0873235 281 162 22 27 152 0.6499999 23 26 178 0.7368418 24 25 -1 -0.0397546 -1 -0.117838 -1 0.015546 -1 0.0770565"
   "-1 0.0653924 29 23 0.9595593 1 10 4 0.1327053 2 3 -1 -0.00174934 125 -0.6576296 4 7 108 62500 5 6 -1 -0.0132271 -1 0.118346 207 0.1592087 8 9 -1 0.00466361 -1 0.0315195 28 0.8386994 11 12 -1 0.04056 232 -0.04360598 13 28 282 37 14 21 50 0.4537006 15 16 -1 -0.00571857 36 0.5084745 17 18 -1 -0.0494344 193 -0.2509114 19 20 -1 0.477875 -1 0.0771215 233 0.1370143 22 27 172 -1.541666 23 26 206 0.4999995 24 25 -1 -0.0573722 -1 0.202265 -1 -0.0574168 -1 0.0532353 -1 -0.0520991 29 30 0 1 22 107 -0.9230768 2 9 0 -1.472005 3 4 -1 -0.0231588 175 -1.047619 5 6 -1 -0.112129 92 -0.8842599 7 8 -1 0.0716402 -1 -0.00114375 210 0.732365 10 19 62 -0.3333329 11 16 26 0.00208191 12 15 222 0.1111111 13 14 -1 0.0607473 -1 -0.0648583 -1 -0.0330322 222 -0.9456521 17 18 -1 0.0292359 -1 -0.0572181 226 0.04464284 20 21 -1 0.176571 -1 -0.0041514 137 1.249998 23 24 -1 0.0028008 113 0.1442307 25 28 105 -0.7419354 26 27"
   "-1 -0.0539955 -1 0.0427296 -1 -0.0594624 29 247 0.3124999 1 8 23 0.2791596 2 7 193 0.1944482 3 4 -1 -0.00102253 230 0.0330891 5 6 -1 0.0173274 -1 0.0744325 -1 -0.00469913 223 0.6249994 9 28 235 -0.04671638 10 11 -1 -0.00629027 117 -0.2645879 12 17 99 0.5215423 13 16 253 -0.06111393 14 15 -1 0.00927868 -1 0.0548974 -1 0.160138 277 6.5 18 27 93 0.201275 19 22 210 0.2450409 20 21 -1 -0.0330412 -1 0.025158 280 0.4383562 23 26 157 -0.05882352 24 25 -1 0.212911 -1 0.0165244 -1 -0.000407921 -1 0.0541441 -1 -0.0130899 29 252 0.1562499 1 28 14 -0.2499999 2 23 200 0.7499995 3 22 159 -0.1212121 4 21 223 0.999998 5 20 26 0.9296718 6 19 3 -0.2768022 7 16 70 -0.4608328 8 13 170 -1.280111 9 10 -1 0.0667239 59 -0.6153846 11 12 -1 -0.0562348 -1 0.00854588 135 -0.5555553 14 15 -1 0.128916 -1 0.0179289 17 -1.285714 17 18 -1 0.117871 -1 -0.0228541 -1 0.0255208 -1 -0.0470076 -1 -0.0178555 -1 0.0197499 97 -0.7952945"
   "24 25 -1 -0.0369854 103 -5.791663 26 27 -1 0.215224 -1 0.00887189 -1 -0.00566252 29 228 0.2272726 1 2 -1 0.0022388 24 0.007898756 3 4 -1 -0.0713269 25 0.005729491 5 8 113 -0.3749999 6 7 -1 -0.0753896 -1 0.0673721 143 -0.7058209 9 18 219 -0.4249998 10 11 -1 -0.0556765 280 0.5919283 12 17 281 57 13 14 -1 0.150571 36 0.956848 15 16 -1 0.0363416 -1 -0.0548962 -1 -0.0349677 281 221 19 26 106 -0.8749997 20 21 -1 0.0486332 92 -0.3897821 22 23 -1 -0.0346742 102 0.4673278 24 25 -1 -0.0124239 -1 0.0301477 163 0.1778342 27 28 -1 0.148157 -1 -0.103031 29 215 1.204735 1 24 169 0.9002353 2 11 139 0.468045 3 8 163 -3.586526 4 7 190 -0.03225792 5 6 -1 -0.00261587 -1 0.0689141 -1 -0.00246042 184 0.4329401 9 10 -1 0.0455906 -1 -0.00586261 177 0.5499996 12 19 64 -0.698795 13 16 101 -0.4171362 14 15 -1 -0.0779382 -1 0.0916729 7 -0.7315587 17 18 -1 -0.0416726 -1 0.0378415 187 -0.01773373 20 23 205 -0.3571427"
   "21 22 -1 0.00461923 -1 0.152955 -1 -0.0338683 225 0 25 28 149 -0.2727272 26 27 -1 0.0750643 -1 -0.072588 -1 -0.0865803 29 247 -1.249999 1 14 210 -0.06243094 2 11 144 -0.2045436 3 4 -1 -0.0365025 95 0.1123579 5 10 201 1.374998 6 9 237 0.1509444 7 8 -1 0.145754 -1 -0.0290655 -1 -0.0429161 -1 -0.0042124 84 0.08695651 12 13 -1 -0.0233921 -1 0.0609533 250 0.02857142 15 16 -1 -0.00471812 112 0.07142853 17 24 127 1.399996 18 23 238 -0.5424589 19 22 160 0.01515151 20 21 -1 -0.00467198 -1 0.112661 -1 0.00689567 -1 0.0774031 21 0.3124999 25 28 81 0.09999996 26 27 -1 -0.0360864 -1 -0.00304594 -1 0.00929007 29 182 1.657894 1 26 138 0.6486447 2 17 95 0.4958109 3 4 -1 -0.000769463 105 0.1041666 5 16 167 0.1489353 6 9 125 0.3227529 7 8 -1 0.0473036 -1 -0.0452334 68 0.3999997 10 15 225 0.3124999 11 14 127 0.4571426 12 13 -1 -0.117565 -1 0.0675334 -1 0.000961714 -1 0.0357525 -1 0.0432327 126 0.6666662 18"
   "25 282 60 19 22 35 0.3870967 20 21 -1 -0.114233 -1 -0.0299905 217 -12.72525 23 24 -1 -0.118203 -1 -0.00696587 -1 0.0436942 26 0.2570495 27 28 -1 -0.0518957 -1 0.0657777 29 43 0.6428571 1 28 45 0.5641025 2 15 199 -1.49375 3 6 112 -0.6842102 4 5 -1 -0.101655 -1 -0.0105409 131 -0.04999996 7 10 186 0.2808377 8 9 -1 -0.0179052 -1 0.0158349 280 0.9323308 11 12 -1 0.00131213 197 0.06249998 13 14 -1 -0.00623599 -1 0.0713425 225 -0.05555554 16 23 282 192 17 22 229 -1.423728 18 19 -1 0.0903308 42 0.4821803 20 21 -1 -0.00386826 -1 0.0353451 -1 -0.0260714 102 0.3607584 24 27 177 -0.2499998 25 26 -1 0.0277035 -1 -0.0144389 -1 0.0222262 -1 -0.00580856 29 65 1.599999 1 28 226 0.7874997 2 15 228 1.089286 3 12 218 0.3124999 4 5 -1 -0.00112612 26 0.7793015 6 11 141 -0.7426208 7 10 139 -0.552987 8 9 -1 -0.0151868 -1 0.125253 -1 0.029282 -1 -0.0594343 70 -1.694782 13 14 -1 0.0912512 -1 -0.052549 179 -0.4210525"
   "16 21 41 0.3636362 17 20 211 -0.2574751 18 19 -1 -0.0219739 -1 0.0599996 -1 -0.0621008 186 -0.04750494 22 25 23 0.200185 23 24 -1 -7.48147e-05 -1 -0.0798246 278 3.5 26 27 -1 0.0492771 -1 -0.0343941 -1 0.0169208 29 206 1.194444 1 24 206 0.8139532 2 11 185 0.7363017 3 10 93 0.4526093 4 5 -1 -0.000828492 95 0.1997917 6 9 130 0.1136363 7 8 -1 0.0384687 -1 0.229787 -1 0.00974255 -1 -0.0296911 218 0.6964282 12 23 199 0.2727272 13 20 72 -1.521708 14 17 217 -0.6075428 15 16 -1 -0.0298195 -1 0.123872 246 -0.1874999 18 19 -1 0.048475 -1 -0.0179031 148 0.06301718 21 22 -1 0.163877 -1 0.0197302 -1 -0.0758434 135 1.249998 25 28 151 -0.2033898 26 27 -1 0.0290629 -1 -0.0631948 -1 0.089134 29 102 0.02891709 1 12 31 0.603701 2 9 32 0.4877471 3 8 282 176 4 5 -1 -0.0109182 38 0.9399999 6 7 -1 -0.0143044 -1 0.0808997 -1 -0.0376601 107 0.7222222 10 11 -1 0.00042245 -1 0.0600689 191 0.6422482 13 24 206 -1.149122"
   "14 19 166 -0.4066872 15 16 -1 0.00805053 184 -0.2584802 17 18 -1 -0.0511648 -1 -0.198336 111 -0.2083333 20 21 -1 0.0259293 171 -0.2490333 22 23 -1 -0.0123413 -1 0.0101137 92 -0.01214797 25 26 -1 -0.0408773 144 0.4411734 27 28 -1 0.0428152 -1 -0.12104 29 187 0.3900655 1 2 -1 0.00102504 71 1.187252 3 28 202 0.1111111 4 7 3 0.6465642 5 6 -1 -0.0845584 -1 0.0411662 200 1.166665 8 27 197 0.8708332 9 24 176 -0.9722221 10 13 148 -0.4279842 11 12 -1 0.103937 -1 -0.0264772 158 -0.3050847 14 19 208 -0.1945392 15 16 -1 -0.0361527 117 -0.3071271 17 18 -1 -0.0621915 -1 0.0928886 43 0.09615383 20 21 -1 0.0298238 282 35 22 23 -1 0.0439501 -1 -0.0580838 130 0.3627451 25 26 -1 0.114155 -1 -0.065651 -1 0.0379076 -1 0.0681238 29 137 -1.083333 1 4 58 -0.2222222 2 3 -1 -0.0369817 -1 0.0201292 219 -0.9999998 5 10 159 0.3181817 6 9 39 0.7142855 7 8 -1 0.0259805 -1 -0.0749872 -1 0.0860045 224 -0.7142853 11 20 191"
   "0.2028863 12 19 108 0.999996 13 18 79 -2.151426 14 15 -1 0.0414169 0 -2.055865 16 17 -1 0.0849579 -1 -0.0395945 -1 -0.105382 -1 0.0411518 163 -0.6717363 21 22 -1 0.00948504 228 -0.5624998 23 26 136 -0.8636362 24 25 -1 0.022885 -1 -0.0555064 281 104 27 28 -1 0.00407894 -1 -0.0132987 29 199 0.1648936 1 4 281 134 2 3 -1 0.000580173 -1 -0.0174218 228 0.4473683 5 28 197 -0.15625 6 21 186 0.1129125 7 12 28 0.5057388 8 11 5 -0.4084534 9 10 -1 0.0633482 -1 0.295784 -1 0.0321272 108 1.15 13 20 24 0.9741083 14 15 -1 0.00151254 118 0.1290258 16 19 53 0.07710641 17 18 -1 -0.00160935 -1 0.404986 -1 -0.0450769 -1 0.140454 119 0.2135254 22 25 100 0.4975294 23 24 -1 -0.0165242 -1 0.0512379 152 0.7205881 26 27 -1 0.0323535 -1 -0.0557236 -1 -0.0185814 29 233 -0.2639627 1 10 277 2.5 2 7 260 0.2020905 3 6 123 -1.144587 4 5 -1 0.0457083 -1 -0.0313819 -1 0.0240615 256 -0.1549284 8 9 -1 0.0385388 -1 0.00730416"
   "38 0.3220339 11 20 216 -0.8127826 12 17 215 -1.1675 13 14 -1 -0.0460832 220 -0.4374999 15 16 -1 0.129527 -1 -0.01964 182 1.083333 18 19 -1 -0.0374132 -1 0.0584072 201 0.1249998 21 24 270 -0.8199997 22 23 -1 -0.0261867 -1 0.00924022 188 0.7774773 25 28 195 -0.01515151 26 27 -1 0.00428879 -1 -0.0158428 -1 0.0359906 29 6 -0.2272711 1 22 202 0.7291662 2 19 45 0.6562499 3 10 131 -0.2499995 4 7 126 1.171428 5 6 -1 -0.0172307 -1 0.0526278 90 -0.7499993 8 9 -1 0.0237479 -1 -0.00295503 108 -0.1666666 11 12 -1 0.0438631 281 139 13 16 254 0.05048877 14 15 -1 0.00555157 -1 -0.0585129 59 -0.3749999 17 18 -1 0.0941508 -1 0.00172975 44 0.735294 20 21 -1 0.0435978 -1 -0.0204238 215 -1.388185 23 24 -1 -0.0394507 82 0.9999996 25 26 -1 -0.000386666 42 0.4047619 27 28 -1 0.0184625 -1 -0.0429328 29 178 0.1785714 1 2 -1 0.00273762 177 0.03571428 3 4 -1 -0.027267 174 -0.05555555 5 14 204 -0.09999992 6 7 -1 -0.0067036"
   "203 0.6249997 8 13 216 0.5892009 9 12 147 0.3650921 10 11 -1 0.0819897 -1 0.0171847 -1 -0.0157789 -1 -0.0413552 197 -0.09740258 15 24 183 0.9636361 16 23 121 0.8362017 17 22 223 1.14 18 19 -1 -0.00984409 199 0.0297619 20 21 -1 0.0993449 -1 -0.0898253 -1 0.0612746 -1 0.0972528 143 0.5433593 25 26 -1 -0.0321304 157 -0.1020408 27 28 -1 -0.0525249 -1 0.0542392 29 104 1.491525 1 20 241 0.2499999 2 3 -1 -0.00318679 227 1.333331 4 19 127 0.999998 5 14 264 0.1944444 6 7 -1 0.000685983 113 -1.2 8 9 -1 -0.0361379 266 0.5156249 10 11 -1 0.0144125 213 -0.1199994 12 13 -1 -0.00212491 -1 0.0993973 202 0.1730768 15 18 211 0.3162826 16 17 -1 0.0166134 -1 0.0884954 -1 -0.000571108 -1 -0.0420393 217 -0.8583064 21 22 -1 -0.0454213 219 -0.3499999 23 26 81 0.7777774 24 25 -1 0.246747 -1 -0.0246852 110 -0.8181815 27 28 -1 -0.0626549 -1 0.0423037 29 31 0.1874086 1 24 36 0.391705 2 23 281 226 3 22 150 -1.2 4 5 -1"
   "0.0682275 0 -0.1984369 6 17 227 -1.499999 7 10 217 -2.570333 8 9 -1 0.13425 -1 -0.0610706 5 -0.401389 11 14 205 0.3114753 12 13 -1 -0.0708046 -1 0.0095778 193 -0.450801 15 16 -1 0.121073 -1 -0.0399351 27 0.3910717 18 21 280 0.07727273 19 20 -1 -0.076109 -1 0.0513441 -1 -0.0765621 -1 0.0742389 -1 -0.0707451 18 -0.999996 25 28 199 0.4038461 26 27 -1 0.0335972 -1 -0.0562209 -1 -8.31086e-05 29 197 -0.4758064 1 28 192 -0.4235567 2 27 47 0.2030681 3 22 199 0.303125 4 15 195 -0.04807691 5 10 135 -0.3181817 6 7 -1 -0.0311313 102 -0.21809 8 9 -1 -0.0296924 -1 0.0232492 128 -0.2205882 11 14 218 0.02499999 12 13 -1 0.011093 -1 0.0877476 -1 -0.0198485 193 -1.260373 16 17 -1 0.0520852 153 0.7812496 18 21 72 -1.134892 19 20 -1 0.0455778 -1 -0.0664272 -1 0.016967 161 0.4148771 23 26 217 -0.4214291 24 25 -1 0.164629 -1 0.0350875 -1 -0.0720642 -1 -0.0338779 -1 0.00181541 29 138 1.09791 1 28 139 0.6741889"
   "2 21 48 0.1208947 3 16 16 -1.53846 4 5 -1 0.0585083 182 -1.343749 6 7 -1 -0.026037 176 0.8305084 8 11 15 0.4999999 9 10 -1 -0.000128314 -1 0.0732418 99 -0.0221731 12 13 -1 0.0750758 153 -0.09090908 14 15 -1 0.135806 -1 -0.075103 277 2.5 17 20 76 -0.6913255 18 19 -1 0.0947741 -1 -0.0220649 -1 -0.0419885 202 0.3999997 22 23 -1 0.0687339 70 -0.0814817 24 27 222 -0.6153845 25 26 -1 -0.0887427 -1 0.0444196 -1 -0.0542058 -1 -0.0553638 29 274 -1.840908 1 14 42 0.7116788 2 3 -1 -0.0104918 257 -0.9928684 4 7 35 0.9259258 5 6 -1 -0.0848293 -1 0.06924 17 -0.1428571 8 11 139 0.3631377 9 10 -1 0.0568135 -1 -0.0814082 163 0.4241423 12 13 -1 0.139729 -1 -0.0396779 279 16.5 15 22 222 0.3862275 16 17 -1 -0.00061988 153 0.7499998 18 21 186 1.156168 19 20 -1 0.0281835 -1 0.151065 -1 -0.0591128 92 0.8135559 23 28 222 -0.1377551 24 27 174 0.3506493 25 26 -1 -0.00382906 -1 0.0317818 -1 -0.0127816 -1 0.0556417"
   "29 158 0.2916665 1 20 55 0.1988469 2 3 -1 0.00134684 282 44 4 13 229 0.06944441 5 12 178 -0.3249999 6 11 26 0.9680818 7 8 -1 -0.0937049 196 0.09999996 9 10 -1 -0.412747 -1 0.0305857 -1 -0.0500198 -1 -0.0153378 150 -0.02941175 14 15 -1 -0.0251914 125 -1.387779e-17 16 19 71 -0.95287 17 18 -1 -0.0253566 -1 0.0339203 -1 -0.0228572 222 0.1914893 21 24 202 0.7499985 22 23 -1 0.0152423 -1 -0.0164213 59 0.4666665 25 26 -1 -0.0279468 144 0.5694419 27 28 -1 0.0444297 -1 -0.054103 29 273 0.1166666 1 28 43 0 2 21 24 0.886733 3 16 40 0.03846153 4 5 -1 0.0408536 38 0.9795918 6 9 207 -1.187926 7 8 -1 0.0424111 -1 -0.0370361 282 198 10 13 179 0.2499997 11 12 -1 0.072433 -1 -0.0374617 125 0.271632 14 15 -1 0.0647266 -1 -0.106643 279 15.5 17 18 -1 0.00656304 175 -0.3448274 19 20 -1 -0.0597463 -1 0.192556 34 0 22 27 136 0.8876403 23 26 281 16 24 25 -1 0.0688041 -1 0.00154121 -1 0.139232 -1 -0.00148231 -1 -0.00441381"
   "29 63 -0.285714 1 26 30 0.09759956 2 15 95 -0.6995791 3 4 -1 0.00188519 99 -0.4867855 5 8 161 -1.185954 6 7 -1 -0.102251 -1 0.0538019 197 0.6312499 9 14 254 0.8260875 10 13 280 0.9639175 11 12 -1 0.0292635 -1 -0.0529197 -1 -0.0513295 -1 -0.0467775 178 -0.7199999 16 17 -1 -0.0570041 74 -0.9143345 18 19 -1 0.0403499 136 -0.05555553 20 21 -1 -0.0341412 213 0.9913745 22 25 217 0.06356479 23 24 -1 -0.0191717 -1 0.0500384 -1 0.125543 156 0.3999998 27 28 -1 -0.00896323 -1 0.00896035 29 241 -1.216666 1 2 -1 0.0151351 244 -0.2125 3 12 64 -0.9999992 4 5 -1 -0.0479668 177 0.06249994 6 7 -1 0.00516634 248 -1.249998 8 9 -1 0.0664465 158 0.4166666 10 11 -1 -0.00885409 -1 -0.0374333 267 0.2115384 13 14 -1 -0.00152855 256 0.01169596 15 24 172 0.1388888 16 23 277 3.5 17 22 107 0.1020408 18 19 -1 0.0547265 156 0.3749993 20 21 -1 0.201583 -1 -0.0267413 -1 0.019349 -1 0.00564796 107 -0.1904762 25 28 281 17 26"
   "27 -1 0.149317 -1 0.00943758 -1 -0.0295015 29 233 0.6698443 1 22 38 1.111111 2 19 109 -0.7249997 3 14 228 -0.6779659 4 9 71 0.3753918 5 6 -1 -0.018758 112 -0.5714282 7 8 -1 -0.00155286 -1 0.124372 154 1.499994 10 13 141 0.1309593 11 12 -1 -0.0576393 -1 0.0339708 -1 0.0966012 122 1.015914 15 18 209 0.710441 16 17 -1 0.000303739 -1 0.0268876 -1 -0.031033 60 0.4210525 20 21 -1 -0.0750883 -1 0.0515716 254 -0.1277917 23 24 -1 -0.0415974 179 -0.2999998 25 26 -1 -0.0285258 134 -0.5925924 27 28 -1 -0.0589395 -1 0.0425797 29 199 1.512499 1 28 280 0.02836879 2 13 281 134 3 8 163 0.2868185 4 7 42 0.5405405 5 6 -1 -0.101677 -1 -0.00286327 -1 0.0715161 26 0.9935575 9 12 96 -1.368294 10 11 -1 -0.0157818 -1 0.0730241 -1 -0.0437568 201 -0.5178568 14 27 281 233 15 24 155 -0.7333331 16 21 218 -0.2499997 17 20 138 -0.2334223 18 19 -1 0.0330483 -1 0.200103 -1 -0.00472752 48 -0.6634105 22 23 -1 -0.0488558 -1"
   "-0.00864566 198 -0.9814812 25 26 -1 0.065445 -1 -0.117115 -1 0.00132038 -1 -0.0486765 29 88 1.299999 1 12 226 0.8402776 2 9 228 1.089286 3 4 -1 0.000771098 70 -1.694782 5 6 -1 0.0875876 144 0.6185526 7 8 -1 -0.0725204 -1 0.0302337 182 0.9166663 10 11 -1 -0.0319143 -1 0.0314243 31 0.3823227 13 20 197 0.4821428 14 17 136 -0.2954544 15 16 -1 0.00421223 -1 0.0814282 23 0.1280307 18 19 -1 0.0452164 -1 -0.0819913 56 1.11003 21 28 185 0.1538444 22 23 -1 -0.0195763 217 -0.3852833 24 27 135 0.3281249 25 26 -1 -0.0663757 -1 0.0509327 -1 0.056843 -1 0.128149 29 168 1.138015 1 28 94 2.168237 2 25 165 -1.33396 3 4 -1 -0.0343355 281 27 5 24 176 -0.6928571 6 7 -1 -0.0408016 107 -0.1904762 8 9 -1 0.0515144 161 -0.1705174 10 23 94 0.2676699 11 14 29 0.9382912 12 13 -1 -0.0355632 -1 0.110364 113 0.3653845 15 18 104 0.7812496 16 17 -1 0.0110834 -1 0.282805 186 0.3696042 19 22 136 0.517857 20 21 -1 0.3315 -1"
   "-0.0309598 -1 -0.0283759 -1 -0.013312 -1 -0.000577122 220 0.1125 26 27 -1 -0.0331082 -1 0.1269 -1 -0.039731 29 26 0.9557858 1 22 232 -0.06209254 2 5 140 0.3203852 3 4 -1 0.0118529 -1 -0.0105821 30 0.1340673 6 9 134 -0.8199999 7 8 -1 0.0088218 -1 -0.0529847 156 -0.015625 10 11 -1 0.00538183 195 0.5999998 12 21 188 -0.03621243 13 14 -1 -0.035603 86 -0.4999995 15 16 -1 -0.0510115 139 0.3575103 17 20 198 -0.2386363 18 19 -1 -0.0675359 -1 0.0247912 -1 -0.0271239 -1 0.0238551 227 1.249999 23 28 153 -0.01960784 24 25 -1 0.0278779 195 -0.5714284 26 27 -1 0.044431 -1 -0.00741554 -1 -0.0753534 29 32 0.03929301 1 4 211 -2.899785 2 3 -1 0.142654 -1 -0.0656119 10 -5.284679 5 8 188 -0.007677739 6 7 -1 0.0596159 -1 -0.0249104 7 -2.906965 9 10 -1 -0.0287339 38 0.02127659 11 24 24 0.1453956 12 19 87 -1.37931 13 16 179 -0.6696428 14 15 -1 -0.0406969 -1 0.224986 44 0 17 18 -1 0.0397019 -1 -0.0411451 108 0.2142856"
   "20 23 126 0.4772726 21 22 -1 0.0256748 -1 0.149973 -1 -0.0325955 26 0.01726875 25 28 40 0.04347825 26 27 -1 0.0870045 -1 -0.0468663 -1 0.000937158 29 239 0.3006285 1 26 28 0.932647 2 23 146 0.1930851 3 8 7 0.09472915 4 5 -1 -0.00281547 23 0.3584156 6 7 -1 0.0415263 -1 -0.0586552 217 0.01877689 9 14 17 -0.9538459 10 13 172 -0.6607141 11 12 -1 -0.0963564 -1 -0.0123107 -1 0.00668753 282 83 15 20 25 0.4636729 16 17 -1 0.0407511 229 0.1770833 18 19 -1 0.181083 -1 0.00697157 278 3.5 21 22 -1 0.0566732 -1 -0.0187847 132 -0.9142853 24 25 -1 0.0740207 -1 -0.027568 261 -0.1322211 27 28 -1 0.0188075 -1 -0.00300688 29 221 -1.714285 1 2 -1 -0.0602939 280 0.05309735 3 10 222 -6.938894e-18 4 5 -1 -0.0509899 97 -1.075638 6 7 -1 0.0437598 207 -0.6588028 8 9 -1 0.0990845 -1 -0.0568464 217 0.7279838 11 14 198 -0.2016129 12 13 -1 0.0061022 -1 -0.00244449 224 -0.08333322 15 20 261 -0.7091974 16 19 145 0.08419104"
   "17 18 -1 0.185563 -1 -0.0334739 -1 -0.0330927 79 0.883774 21 26 212 -0.3846794 22 25 282 78 23 24 -1 0.149788 -1 -0.0173702 -1 -0.0625088 199 0.0297619 27 28 -1 0.356949 -1 0.0417292 29 275 0.6315788 1 24 120 0.3708301 2 13 4 0.06910224 3 4 -1 0.00294683 146 -0.3354741 5 8 150 0.5227271 6 7 -1 -0.0532244 -1 0.0702412 106 -0.1041666 9 10 -1 -0.0356054 253 0.5802609 11 12 -1 -0.000220936 -1 0.0723855 25 0.2340678 14 15 -1 -0.0124064 282 31 16 17 -1 -0.03834 219 0.6562493 18 21 223 -0.8749965 19 20 -1 -0.0959702 -1 0.0380866 150 0.6341461 22 23 -1 -0.033795 -1 0.0445745 237 -0.1465693 25 28 174 0.2371795 26 27 -1 0.0235946 -1 -0.0213692 -1 -0.021511 29 84 0.4673913 1 10 223 -0.04999996 2 3 -1 -0.00537627 213 -0.3106038 4 9 117 0.5211037 5 8 198 0.6428568 6 7 -1 0.0180672 -1 -0.0369665 -1 0.0801654 -1 0.000668336 223 0.08333322 11 26 44 0.1445783 12 15 203 0.08287292 13 14 -1 -0.00293443 -1 -0.0945405"
   "8 -0.1819622 16 25 213 -0.06249962 17 24 78 -0.5059851 18 19 -1 0.0308051 281 147 20 23 221 -0.7421873 21 22 -1 0.0238826 -1 -0.0808767 -1 0.0611386 -1 0.052817 -1 -0.0190535 224 0.9749992 27 28 -1 -0.0276608 -1 0.0468057"]))

(def qlc-ml-model
  ;; traeerne laest én gang: {:classes [...] :base doubles :trees [[iteration: K traeer]]}, hvert trae fire
  ;; arrays (feature, taerskel/vaerdi, venstre, hoejre). Fejler noget her, er modellen der ikke (nil) -
  ;; og analysen er analyse 10 som foer
  (delay
    (try
      (let [tok  (java.util.Scanner. ^String qlc-ml-model-text)
            nxt  (fn [] (.next tok))
            K    (Long/parseLong (nxt))
            cls  (vec (repeatedly K nxt))
            n    (Long/parseLong (nxt))
            base (double-array (repeatedly K #(Double/parseDouble (nxt))))
            tree (fn []
                   (let [c  (Long/parseLong (nxt))
                         fe (int-array c) th (double-array c) le (int-array c) ri (int-array c)]
                     (dotimes [i c]
                       (let [f (Long/parseLong (nxt))]
                         (aset fe i (int f))
                         (if (neg? f)
                           (aset th i (Double/parseDouble (nxt)))
                           (do (aset th i (Double/parseDouble (nxt)))
                               (aset le i (int (Long/parseLong (nxt))))
                               (aset ri i (int (Long/parseLong (nxt))))))))
                     [fe th le ri]))
            trees (vec (repeatedly n (fn [] (vec (repeatedly K tree)))))]
        {:classes cls :base base :trees trees})
      (catch Throwable e
        (try (timbre/warn "QLC+ sektionsmodellen kunne ikke laeses:" (.getMessage e)) (catch Throwable _ nil))
        nil))))

(defn qlc-ml-tree ^double [[^ints fe ^doubles th ^ints le ^ints ri] ^doubles x]
  (loop [i 0]
    (let [f (aget fe i)]
      (if (neg? f)
        (aget th i)
        (recur (if (<= (aget x f) (aget th i)) (aget le i) (aget ri i)))))))

(defn qlc-ml-proba
  "sandsynligheden for hver tilstand i én takt (softmax over traeernes sum)"
  [model x]
  (let [x   (double-array x)
        K   (count (:classes model))
        raw (aclone ^doubles (:base model))]
    (doseq [it (:trees model) k (range K)]
      (aset raw k (+ (aget raw k) (qlc-ml-tree (nth it k) x))))
    (let [mx (reduce max (seq raw))
          ex (mapv #(Math/exp (- % mx)) (seq raw))
          s  (reduce + ex)]
      (mapv #(/ % s) ex))))

(defn qlc-ml-viterbi
  "den mest sandsynlige raekke af tilstande: et skift koster, mindre paa en 4-takters linje"
  [probs K]
  (let [sw   (qlc-opt :ml-switch 3.0)
        lp   (mapv (fn [p] (mapv #(Math/log (+ % 1e-6)) p)) probs)
        m    (count lp)]
    (when (pos? m)
      (loop [j 1 score (nth lp 0) backs []]
        (if (< j m)
          (let [pen   (* sw (if (zero? (mod j 4)) 0.35 1.0))
                best  (mapv (fn [b]
                              ;; den bedste forgaenger til b (foerste ved lighed, som numpy's argmax)
                              (reduce (fn [[bi bv] a]
                                        (let [v (- (nth score a) (if (= a b) 0.0 pen))]
                                          (if (> v bv) [a v] [bi bv])))
                                      [0 (- (nth score 0) (if (= 0 b) 0.0 pen))]
                                      (range 1 K)))
                              (range K))]
            (recur (inc j) (mapv (fn [b] (+ (second (nth best b)) (nth (nth lp j) b))) (range K))
                   (conj backs (mapv first best))))
          (let [last-k (first (reduce (fn [[bi bv] k] (if (> (nth score k) bv) [k (nth score k)] [bi bv]))
                                      [0 (nth score 0)] (range 1 K)))]
            (vec (reverse (reduce (fn [path bk] (conj path (nth bk (peek path))))
                                  [last-k] (reverse backs))))))))))

(def qlc-ml-type {"BREAK" "break" "GROOVE" "normal" "BUILD" "build" "DROP" "drop"})

(defn qlc-ml-states
  "modellens tilstand pr. takt, og takternes starter - nil uden model"
  [curves grid]
  (when-let [model @qlc-ml-model]
    (let [{:keys [starts X]} (qlc-ml-features curves grid)
          K     (count (:classes model))
          path  (qlc-ml-viterbi (mapv #(qlc-ml-proba model %) X) K)]
      (when path
        {:starts starts :states (mapv #(nth (:classes model) %) path)}))))

(defn qlc-ml-markers-of
  "R526_MARKERS_OF: sektionerne som markoerer fra en faerdig sti (som analyse 10's: 1-baserede beats, type,
   energi) - nil uden mindst to sektioner og et drop"
  [curves n starts states]
  (when (seq states)
    (let [n      (long n)
          energy (:energy curves)
          runs   (reduce (fn [acc [j st]]
                           (let [l (peek acc)]
                             (if (and l (= (:state l) st))
                               acc
                               (conj acc {:state st :from (nth starts j)}))))
                         [] (map-indexed vector states))
          mk     (vec (map-indexed
                        (fn [i r]
                          (let [a (:from r)
                                b (if (< (inc i) (count runs)) (:from (nth runs (inc i))) n)]
                            {:beat   (if (zero? i) 1 (inc a))
                             :type   (get qlc-ml-type (:state r) "normal")
                             :energy (double (qlc-mean-range energy a b))}))
                        runs))]
      (when (and (>= (count mk) 2) (some #(= "drop" (:type %)) mk))
        mk))))

(defn qlc-ml-markers
  "R525_ML: sektionerne som markoerer (R526_MARKERS_WRAP: via qlc-ml-markers-of)"
  [curves grid]
  (when-let [{:keys [starts states]} (qlc-ml-states curves grid)]
    (qlc-ml-markers-of curves (.-beatCount grid) starts states)))

;; ---------------------------------------------------------------- R526_FINE_BLOCK: finere haendelser (punkt 3)
;;
;; Tobias 11-01 ("3 er den usikre faktor ... slaa punkt 3 til og fra inde i selve programmet"): en model pr.
;; BEAT for det Time-line markerer inde i sektionerne - pre-drop, dips i dropet (drop-break / drive-break) og
;; outro - oven paa sektionsmodellens egen sti. Paa de 61 testnumre (F1 mod Time-line, +-2 beats): outro
;; 0.89, dips 0.65-0.71, pre-drop 0.45-0.50. Rhythmic-build er udeladt (F1 0.17). Sendes som :events; QLC+
;; bruger dem kun naar AI DETAILS er slaaet til (SETUP > ADVANCED).

(def qlc-ml-fine-text
  ;; samme format som qlc-ml-model-text; klasserne DIP NONE OUT PRE
  (clojure.string/join " " [
   "4 DIP NONE OUT PRE 100 -0.0406483 2.35112 -0.844192 -1.46628 29 30 0.6493225694 1 18 26 0.1593982279 2 11 191 0.6177805066 3 4 -1 -0.0728694 307 60 5 6 -1 -0.023184 303 0.006296726409 7 8 -1 -0.0337574 305 0.1329652667 9 10 -1 0.0538526 -1 0.591116 304 0.02367858589 12 13 -1 -0.029979 301 0.5 14 15 -1 0.0202192 191 0.4585247636 16 17 -1 0.288993 -1 0.668741 26 0.08896971494 19 20 -1 0.0711044 307 7 21 24 303 0.7549455166 22 23 -1 0.296272 -1 -0.0424252 23 0.1495531648 25 28 303 0.003490089206 26 27 -1 0.113358 -1 0.572938 -1 0.808578 29 30 0.6493225694 1 20 316 13 2 5 303 0.1808013618 3 4 -1 -0.020937 -1 -0.546543 191 0.6177805066 6 17 303 0.9842739701 7 14 26 0.1593982279 8 9 -1 0.0789761 304 0.02444051765 10 11 -1 0.0491853 301 0.5 12 13 -1 -0.0024242 -1 -0.170275 316 73 15 16 -1 -0.515313 -1 0.0832184 303 0.005082652438 18 19 -1 0.0485114 -1 -0.30322 23 0.1453047246 21 24 303 0.006647820119"
   "22 23 -1 0.0171432 -1 -0.262301 312 1.5 25 28 309 128 26 27 -1 -0.182766 -1 -0.450655 -1 -0.517075 29 303 0.977835238 1 22 316 13 2 15 303 0.7878564 3 8 8 -1.100619316 4 7 31 0.1793162674 5 6 -1 -0.0585133 -1 1.1661 -1 0.0359538 123 0.004351800773 9 14 69 -0.00246252073 10 11 -1 0.126501 304 0.02701509744 12 13 -1 1.97515 -1 0.220459 -1 0.217037 316 20 16 21 303 0.4913332164 17 18 -1 -0.0543611 307 11 19 20 -1 1.38158 -1 0.274289 -1 -0.0812171 316 73 23 28 138 0.01272738818 24 27 54 -0.007181305904 25 26 -1 0.440542 -1 2.14859 -1 0.223957 -1 -0.0800541 29 309 4 1 16 31 0.4126525521 2 11 305 0.02718859538 3 8 153 -0.06666666269 4 5 -1 0.199745 163 0.2758197188 6 7 -1 0.0537989 -1 1.82606 304 0.2833355069 9 10 -1 -0.0166173 -1 0.66225 304 0.4065308571 12 13 -1 0.641607 201 -0.9999989867 14 15 -1 0.282674 -1 3.16876 309 8 17 22 28 0.5428493023 18 21 21 -0.6666665673 19 20 -1 0.710941 -1 0.0713867"
   "-1 2.06301 184 1.21513164 23 24 -1 -0.0622555 304 0.1641772836 25 26 -1 0.0563901 279 0.1217559278 27 28 -1 0.252466 -1 1.05609 29 25 0.2544090152 1 12 191 0.6177805066 2 7 26 0.1218636259 3 4 -1 -0.0728969 313 6.5 5 6 -1 -0.0377159 -1 0.100918 255 -0.07561096549 8 9 -1 -0.000563395 7 -0.4929999709 10 11 -1 0.3002 -1 0.0311196 304 0.01472618431 13 20 38 0.4999999106 14 15 -1 -0.0442092 303 0.8196786046 16 19 24 0.02837673202 17 18 -1 -0.0283807 -1 0.263097 -1 -0.064864 24 -0.08370088041 21 22 -1 0.0357086 307 23 23 26 278 -0.6834509373 24 25 -1 0.0456006 -1 0.206059 191 -0.4914953709 27 28 -1 0.0898926 -1 0.282243 29 25 0.3246926963 1 18 316 18 2 5 303 0.039558433 3 4 -1 0.0616775 -1 -0.17105 191 0.6177805066 6 13 303 0.008045871742 7 8 -1 0.0842849 309 252 9 12 309 8 10 11 -1 -0.0264186 -1 0.0670333 -1 -0.029608 187 0.08069829643 14 15 -1 0.0426544 255 -0.07561096549 16 17 -1 -0.0918143"
   "-1 -0.206604 303 0.003490089206 19 22 38 0.6666665673 20 21 -1 0.0402229 -1 -0.131941 15 -0.07547169179 23 28 278 -0.590696156 24 27 308 11 25 26 -1 -0.202245 -1 -0.0762361 -1 -0.231078 -1 -0.0711877 29 315 0.9569377899 1 14 303 0.977835238 2 9 316 25 3 8 303 0.3083817363 4 5 -1 -0.0742868 247 -0.1666666418 6 7 -1 0.51185 -1 -0.054477 -1 -0.0822609 316 73 10 13 212 -1.175028324 11 12 -1 -0.0427196 -1 0.207384 -1 -0.0793218 303 0.4913332164 15 22 77 0.7580089569 16 19 316 1 17 18 -1 0.416863 -1 -0.0591879 52 0.5555527806 20 21 -1 0.0341692 -1 0.483124 252 0 23 28 264 750000 24 27 308 13 25 26 -1 0.232337 -1 0.286142 -1 -0.00423229 -1 -0.0636657 29 185 0.4381658137 1 14 309 8 2 7 305 0.008730986156 3 6 6 -0.428568989 4 5 -1 0.546796 -1 0.126542 -1 0.0113777 258 1.361362934 8 9 -1 -0.0668168 232 0.2368386537 10 11 -1 0.00145713 231 1.001167417 12 13 -1 -0.037815 -1 1.36757 304 0.08843395114"
   "15 16 -1 -0.0492726 8 -0.2297101617 17 26 308 4 18 19 -1 0.206623 316 382 20 25 186 0.2007712126 21 22 -1 0.0614177 303 0.09437434375 23 24 -1 0.349892 -1 0.787282 -1 0.238041 6 -0.7499952912 27 28 -1 0.335252 -1 0.0915795 29 25 0.1896209419 1 18 186 0.4419252872 2 13 304 0.01201449987 3 4 -1 -0.0791147 254 -0.01044464298 5 6 -1 -0.066057 1 -0.1332823783 7 12 305 0.2235523164 8 9 -1 -0.0211178 313 4.5 10 11 -1 0.0875297 -1 0.42982 -1 -0.0389694 309 32 14 15 -1 -0.0468203 303 0.005082652438 16 17 -1 -0.0442864 -1 0.178508 304 0.01431206148 19 22 313 6.5 20 21 -1 -0.0285601 -1 0.126067 255 -0.1241060197 23 24 -1 0.0498345 15 -0.2499999702 25 28 303 0.2580105364 26 27 -1 0.209111 -1 0.0924122 -1 0.108031 29 25 0.2434236109 1 18 315 0.9678111672 2 15 186 0.4419252872 3 12 303 0.009172229096 4 5 -1 0.0817517 301 0.5 6 7 -1 0.0606704 7 -0.5435100198 8 11 313 4.5 9 10 -1 0.000881036 -1 -0.295637"
   "-1 0.0239813 303 0.005082652438 13 14 -1 0.0587598 -1 -0.100709 303 0.039558433 16 17 -1 0.0570412 -1 -0.138903 24 0.08678533137 19 22 303 0.004395919386 20 21 -1 0.052419 -1 -0.0626804 191 -0.4914953709 23 24 -1 -0.00649836 313 5.5 25 28 309 64 26 27 -1 -0.0341401 -1 -0.137898 -1 -0.17532 29 315 0.9569377899 1 14 303 0.977835238 2 9 316 25 3 8 303 0.3083817363 4 5 -1 -0.0732683 268 0.1874999851 6 7 -1 -0.00760462 -1 0.358552 -1 -0.0819386 316 73 10 13 212 -1.175028324 11 12 -1 -0.0423552 -1 0.172299 -1 -0.0786464 303 0.4913332164 15 24 51 0.3307096958 16 19 316 1 17 18 -1 0.357887 -1 -0.0745475 52 0.4999974966 20 21 -1 -0.030148 133 -0.09999998659 22 23 -1 -0.0250233 -1 0.302149 252 0 25 28 126 0.5555555224 26 27 -1 0.186071 -1 -0.0126877 -1 -0.0647641 29 185 0.4381658137 1 18 309 10 2 9 27 0.3599937856 3 4 -1 -0.00411294 248 0.4062499106 5 8 305 0.03895679489 6 7 -1 0.383572 -1 0.0217186"
   "-1 0.0293101 258 1.237382054 10 13 303 0.02036424167 11 12 -1 -0.079337 -1 -0.0420238 209 0.1683617383 14 15 -1 -0.0241345 281 1.088909268 16 17 -1 -0.0509228 -1 0.80066 304 0.08843395114 19 20 -1 -0.0481077 6 0.1666660458 21 24 316 517 22 23 -1 0.20656 -1 0.080323 13 -0.6666665673 25 28 268 -0.7638887763 26 27 -1 0.0610862 -1 0.279322 -1 0.0385047 29 25 0.1896209419 1 16 186 0.3107814193 2 9 26 0.1218636259 3 6 7 -0.6847823858 4 5 -1 -0.0422167 -1 -0.077027 313 6.5 7 8 -1 -0.0373321 -1 0.0812397 307 57 10 11 -1 -0.0357685 304 0.0208017882 12 13 -1 -0.0452607 305 0.02436156198 14 15 -1 -0.0571311 -1 0.165114 304 0.008132894523 17 18 -1 -0.0245599 24 0.01848887838 19 22 313 6.5 20 21 -1 -0.0240719 -1 0.140479 305 0.2886505127 23 26 313 5.5 24 25 -1 0.0279287 -1 0.131085 145 -0.4277003407 27 28 -1 0.0601996 -1 0.160628 29 26 0.1593982279 1 18 315 0.9710704088 2 15 191 0.5459263921 3 10 303"
   "0.00977383554 4 5 -1 0.0822969 309 252 6 7 -1 0.0581782 306 0.1991266757 8 9 -1 -0.0696978 -1 0.051047 304 0.02145929821 11 12 -1 0.0433882 260 -0.8091381192 13 14 -1 -0.00981224 -1 -0.10085 303 0.01215680223 16 17 -1 0.0635013 -1 -0.12049 38 0.3333332837 19 26 303 0.01043619774 20 23 313 6.5 21 22 -1 0.0592245 -1 -0.0389072 309 64 24 25 -1 -0.00617009 -1 -0.100134 25 0.06073949113 27 28 -1 -0.00542697 -1 -0.124446 29 315 0.9530726075 1 12 303 0.977835238 2 7 316 25 3 6 303 0.5305411816 4 5 -1 -0.0654817 -1 0.24039 -1 -0.0817222 316 73 8 11 4 -1.150647998 9 10 -1 -0.0491863 -1 0.159069 -1 -0.0780009 303 0.4913332164 13 22 77 0.5790207386 14 17 315 0.9975062609 15 16 -1 -0.0645123 -1 0.207462 115 0 18 21 183 -0.1249999851 19 20 -1 0.0409207 -1 0.290765 -1 -0.0341972 252 0 23 28 197 -0.9999995232 24 25 -1 -0.0086484 264 750000 26 27 -1 0.156952 -1 -0.0320023 -1 -0.067119 29 304 0.1565067619"
   "1 8 212 1.356078506 2 5 304 0.06203655526 3 4 -1 -0.0739835 -1 -0.0192966 278 0.3053129613 6 7 -1 -0.0515489 -1 0.562653 279 -0.01040279586 9 16 251 -0.9374997616 10 15 219 -0.07499998808 11 12 -1 -0.0547903 259 -0.04999981821 13 14 -1 -0.0307592 -1 1.08054 -1 -0.0425173 8 -0.1757239252 17 26 309 4 18 19 -1 0.135423 13 -0.4999999404 20 21 -1 0.330399 3 -0.895416379 22 23 -1 0.0409423 274 0.3333332837 24 25 -1 0.103919 -1 0.390743 17 -0.2222221941 27 28 -1 0.122775 -1 0.00328921 29 25 0.1738043427 1 18 186 0.3107814193 2 13 304 0.0114713423 3 4 -1 -0.0767809 305 0.04146851599 5 6 -1 -0.0719395 2 -0.7869145274 7 12 268 -0.2053571343 8 9 -1 -0.0322597 313 4.5 10 11 -1 0.0693719 -1 0.357326 -1 -0.0370887 307 57 14 15 -1 -0.0361685 304 0.0208017882 16 17 -1 -0.046317 -1 0.111043 304 0.005599448923 19 20 -1 -0.044271 24 0.01848887838 21 24 313 6.5 22 23 -1 -0.0268822 -1 0.101523 307 23 25 26 -1"
   "0.0398046 145 -0.523770988 27 28 -1 0.0383457 -1 0.126631 29 15 -0.2666666508 1 18 268 -0.1136363521 2 9 315 0.9605399966 3 8 186 0.294642508 4 5 -1 0.0596497 268 -0.8365383744 6 7 -1 0.00562614 -1 -0.0749757 -1 -0.0927934 303 0.002034741454 10 13 30 0.5796094537 11 12 -1 0.0749692 -1 -0.0942007 309 64 14 17 278 -0.02318211272 15 16 -1 -0.0117366 -1 -0.123375 -1 -0.11984 26 0.1667555422 19 24 303 0.977835238 20 23 187 0.297070086 21 22 -1 0.0772958 -1 0.0168703 -1 -0.100439 304 0.0129307434 25 26 -1 0.042201 256 0.03019007109 27 28 -1 0.0261464 -1 -0.0680079 29 315 0.9530726075 1 10 282 -1.567301035 2 9 316 77 3 8 247 -0.3333332837 4 5 -1 0.200916 283 -0.162109971 6 7 -1 -0.0692385 -1 0.127948 -1 -0.0802279 -1 -0.0814198 303 0.4913332164 11 18 51 0.3307096958 12 15 316 1 13 14 -1 0.228374 -1 -0.0730819 52 0.4999974966 16 17 -1 -0.026126 -1 0.15636 123 0.007260651328 19 28 100 -0.04245723784"
   "20 21 -1 0.0248496 308 13 22 23 -1 0.131807 303 0.977835238 24 27 253 -1.588798046 25 26 -1 -0.0543249 -1 0.312385 -1 0.13828 -1 0.0141223 29 304 0.1565067619 1 12 208 1.105818987 2 9 31 0.9013977647 3 4 -1 -0.0698279 304 0.06203655526 5 6 -1 -0.0340466 283 -0.1450257301 7 8 -1 0.0148093 -1 0.334899 207 0.7390460372 10 11 -1 0.384208 -1 -0.0539862 279 -0.01040279586 13 22 251 -0.9374997616 14 19 210 -0.9635498524 15 16 -1 -0.0327117 254 -0.5177604556 17 18 -1 0.667317 -1 -0.0110812 185 0.3235968947 20 21 -1 -0.0603756 -1 0.0624392 6 -0.3773569167 23 24 -1 0.15919 8 -0.5351192951 25 26 -1 0.125132 237 0.01353795081 27 28 -1 -0.0065839 -1 0.0854273 29 25 0.1529503465 1 16 313 6.5 2 9 304 0.02220107801 3 4 -1 -0.0752756 254 -0.04112828523 5 6 -1 -0.0639944 7 -0.6919954419 7 8 -1 0.108665 -1 -0.0128715 304 0.008989008144 10 11 -1 -0.0609369 301 0.5 12 15 302 0.5 13 14 -1 -0.0569645 -1 0.107504"
   "-1 0.122786 304 0.004999093246 17 18 -1 -0.0493314 24 0.01848887838 19 22 186 0.4666241705 20 21 -1 -0.028687 -1 0.0825283 305 0.3256161213 23 26 312 2.5 24 25 -1 0.0118927 -1 0.0976353 191 -0.209146753 27 28 -1 0.0315722 -1 0.114004 29 15 -0.2666666508 1 18 268 -0.09999998659 2 9 315 0.97430408 3 8 185 0.3938509524 4 5 -1 0.0512875 260 -0.8091381192 6 7 -1 5.27004e-05 -1 -0.0766445 -1 -0.0904885 303 0.002034741454 10 13 30 0.5796094537 11 12 -1 0.0723423 -1 -0.0825573 309 64 14 17 278 -0.605109334 15 16 -1 0.00839796 -1 -0.0887156 -1 -0.104152 26 0.1593982279 19 24 303 0.977835238 20 23 304 0.02367858589 21 22 -1 0.0793146 -1 0.0461831 -1 -0.0905959 303 0.006296726409 25 26 -1 0.0265666 256 0.07710429281 27 28 -1 0.00851478 -1 -0.075576 29 315 0.9491255879 1 10 282 -1.567301035 2 9 316 77 3 8 211 -0.991733253 4 5 -1 -0.0887368 239 -0.7496654987 6 7 -1 0.171424 -1 -0.0461658 -1 -0.0798457"
   "-1 -0.0812653 303 0.4913332164 11 20 308 2 12 17 197 -0.1999999732 13 14 -1 -0.0220844 202 -0.1249999851 15 16 -1 -0.0566101 -1 0.208357 50 0.3855744004 18 19 -1 -0.0778208 -1 0.0392062 115 0.00456088176 21 28 100 -0.0161959473 22 25 306 0.05012866482 23 24 -1 0.100709 -1 -0.045944 264 750000 26 27 -1 0.124449 -1 -0.0495204 -1 0.0151696 29 304 0.1384251416 1 10 212 1.356078506 2 7 31 1.353889108 3 4 -1 -0.0674959 304 0.02220107801 5 6 -1 -0.0600108 -1 0.209055 305 0.9933157563 8 9 -1 -0.063182 -1 0.329065 279 -0.01040279586 11 18 251 -0.9374997616 12 15 259 -0.02083321102 13 14 -1 -0.056979 -1 0.361819 162 0.2116357684 16 17 -1 -0.0572462 -1 0.0496282 13 -0.2222221941 19 26 16 0.1428571194 20 25 253 0.5850378275 21 24 29 0.4285686612 22 23 -1 0.101593 -1 0.165899 -1 0.330569 -1 0.0169815 245 0.04166665301 27 28 -1 0.00106385 -1 0.0841186 29 25 0.1398992091 1 16 313 6.5 2 11 7 -0.7129709125"
   "3 10 260 -0.3503370881 4 5 -1 -0.0596569 313 4.5 6 7 -1 0.00955037 303 0.6158702374 8 9 -1 0.252733 -1 -0.0141196 -1 -0.068943 304 0.008989008144 12 13 -1 -0.0575466 305 0.02581990324 14 15 -1 -0.0512317 -1 0.0799928 38 0.4999999106 17 22 313 6.5 18 21 304 0.04893706739 19 20 -1 -0.0386601 -1 0.0354554 -1 0.0810967 303 0.7549455166 23 28 278 -0.1317996383 24 27 308 8 25 26 -1 0.0995704 -1 0.0298691 -1 0.104002 -1 -0.0552164 29 303 0.003490089206 1 10 191 0.771566689 2 7 26 0.1218636259 3 4 -1 0.0785386 122 -0.9962091446 5 6 -1 -0.061462 -1 0.0471682 38 0.4999999404 8 9 -1 0.0349429 -1 -0.100122 309 128 11 20 186 0.1407579631 12 17 25 0.2667725384 13 14 -1 0.0563114 312 1.5 15 16 -1 0.0108077 -1 -0.0749039 23 0.02066103555 18 19 -1 -0.000293219 -1 -0.0718868 15 -0.1999999881 21 26 268 -0.2499999553 22 25 10 -2.499924898 23 24 -1 -0.0812711 -1 0.00586495 -1 -0.0962892 254 0.1309258938 27 28"
   "-1 0.0374627 -1 -0.0401934 29 315 0.9491255879 1 12 282 -1.567301035 2 11 316 77 3 10 211 -0.991733253 4 5 -1 -0.0863435 303 0.230530113 6 7 -1 -0.0566788 72 0.207574904 8 9 -1 0.118086 -1 0.362677 -1 -0.0794761 -1 -0.0810263 303 0.4913332164 13 20 308 2 14 17 77 0.382030189 15 16 -1 0.00933931 -1 0.186759 74 0.4160581231 18 19 -1 -0.074245 -1 0.0538844 123 0.004351800773 21 26 100 -0.0161959473 22 23 -1 0.0216343 264 750000 24 25 -1 0.113894 -1 -0.051854 271 0.624999404 27 28 -1 -0.026479 -1 0.151624 29 304 0.1330378056 1 10 212 1.356078506 2 7 304 0.01431206148 3 4 -1 -0.0785029 31 1.353889108 5 6 -1 -0.0448444 -1 0.133517 305 0.9933157563 8 9 -1 -0.0612093 -1 0.271177 279 -0.01040279586 11 18 21 -0.5555555224 12 17 316 247 13 16 217 -1.160379171 14 15 -1 0.523948 -1 0.0692309 -1 0.00272935 -1 -0.0491414 8 -0.1757239252 19 26 316 513 20 25 308 4 21 22 -1 0.0986313 29 0.6666627526 23 24"
   "-1 0.117854 -1 0.235477 -1 0.0318896 17 -0.2222221941 27 28 -1 0.0749826 -1 -0.00984059 29 25 0.1342718154 1 18 313 6.5 2 11 304 0.02220107801 3 4 -1 -0.0724678 305 0.08072589338 5 6 -1 -0.0672488 15 -0.3333333135 7 10 268 -0.3249999583 8 9 -1 -0.0334598 -1 0.108737 -1 -0.03378 306 0.02685395442 12 13 -1 -0.0389673 307 55 14 15 -1 -0.00843889 303 0.003120040754 16 17 -1 -0.0344893 -1 0.102671 24 0.01848887838 19 22 304 0.05516370013 20 21 -1 -0.0449152 -1 0.052718 303 0.7549455166 23 28 191 -0.2269309461 24 25 -1 0.0128913 255 -0.1171765327 26 27 -1 0.0173689 -1 0.0832706 -1 -0.0543403 29 303 0.003490089206 1 10 191 0.771566689 2 7 26 0.1218636259 3 4 -1 0.077005 313 6.5 5 6 -1 0.0504596 -1 -0.029411 161 -0.1508009285 8 9 -1 0.042576 -1 -0.0853177 309 128 11 20 38 0.3333332837 12 17 185 0.3235968947 13 16 308 8 14 15 -1 0.00994788 -1 0.0594316 -1 -0.02646 307 27 18 19 -1 -0.00193235 -1 -0.0749214"
   "15 -0.1333333254 21 26 260 -0.1287943572 22 25 304 0.004304278642 23 24 -1 -0.0858038 -1 -0.0106577 -1 -0.0864175 313 6.5 27 28 -1 0.0220499 -1 -0.0653131 29 315 0.9407407641 1 8 282 -1.567301035 2 7 316 77 3 6 269 0.09374997765 4 5 -1 0.143317 -1 -0.0313936 -1 -0.0790929 -1 -0.081054 303 0.4913332164 9 18 308 2 10 15 197 -0.1999999732 11 12 -1 -0.025764 77 -0.1486852616 13 14 -1 -0.0623828 -1 0.159735 73 0.3064672351 16 17 -1 -0.0759044 -1 0.0318565 115 0.00456088176 19 26 54 -0.007181305904 20 23 261 0.2540774345 21 22 -1 -0.0159288 -1 0.22058 264 750000 24 25 -1 0.105393 -1 -0.0475599 52 0.9999949932 27 28 -1 -0.0242902 -1 0.137108 29 304 0.07844910026 1 6 258 1.237382054 2 3 -1 -0.0678305 263 -0.7368832827 4 5 -1 -0.0474129 -1 0.225463 13 -0.4999999404 7 22 279 -0.5334874988 8 9 -1 -0.0293347 238 -0.8027148843 10 17 309 8 11 12 -1 0.13631 258 -1.323643565 13 14 -1 -0.0402803 298 0.5312498808"
   "15 16 -1 0.620777 -1 0.00351907 28 0.2828944325 18 21 70 -0.2377953976 19 20 -1 0.116044 -1 0.0318687 -1 0.140401 279 0.05223393813 23 26 6 -0.7499969006 24 25 -1 0.0564526 -1 -0.051145 18 -0.2499999404 27 28 -1 0.0833655 -1 0.0109193 29 26 0.1056959108 1 14 313 6.5 2 9 7 -0.6919954419 3 8 260 -0.3371994793 4 5 -1 -0.0565543 306 0.01923140511 6 7 -1 -0.0483223 -1 0.0895654 -1 -0.0664381 7 -0.3455945551 10 13 255 -0.6092097759 11 12 -1 -0.0217531 -1 0.107966 -1 -0.0311989 313 5.5 15 24 305 0.2099691331 16 17 -1 -0.027363 25 -0.004351495765 18 19 -1 -0.0442927 255 -0.06492384523 20 21 -1 -0.0170069 316 379 22 23 -1 0.0777137 -1 0.0127299 145 -0.6610625386 25 26 -1 0.0113525 268 -0.4210525751 27 28 -1 -0.0162763 -1 0.0843182 29 303 0.003490089206 1 10 191 0.6982433796 2 7 122 -0.900801897 3 6 53 -0.7063861489 4 5 -1 0.0606165 -1 -0.0621775 -1 0.0721447 38 0.4999999106 8 9 -1 0.0363053 -1 -0.0767308"
   "309 128 11 20 186 0.1341702938 12 17 25 0.1463803947 13 14 -1 0.0525862 312 2.5 15 16 -1 0.00520187 -1 -0.0647262 23 0.02066103555 18 19 -1 0.00251021 -1 -0.0573939 7 0.03589774668 21 28 260 -0.0510475561 22 27 315 0.9773463011 23 26 185 0.2686583102 24 25 -1 0.0148822 -1 -0.063138 -1 -0.0745488 -1 -0.0765914 -1 0.0212184 29 315 0.9407407641 1 8 282 -1.567301035 2 7 316 77 3 6 269 0.09374997765 4 5 -1 0.130628 -1 -0.0326864 -1 -0.0787018 -1 -0.0808697 303 0.4913332164 9 16 52 0.4999974966 10 13 308 1 11 12 -1 0.0736359 -1 -0.0743949 46 1.071424365 14 15 -1 0.00231837 -1 0.154865 197 -0.1777777672 17 18 -1 0.00752446 252 0 19 28 307 15 20 21 -1 0.104448 310 127 22 25 308 6 23 24 -1 0.0542861 -1 -0.120401 227 0.0624999702 26 27 -1 0.103565 -1 -0.0310872 -1 -0.0727648 29 304 0.07844910026 1 6 235 1.148633599 2 3 -1 -0.0663499 286 -0.5119736195 4 5 -1 -0.073066 -1 0.220072 28 0.4382185638 7 24"
   "71 -0.108654961 8 11 197 0.9999997616 9 10 -1 0.0505151 -1 0.127108 17 -0.3478260338 12 21 13 -1.142856956 13 16 256 -0.7140342593 14 15 -1 0.0678214 -1 0.288061 15 -0.8163264394 17 18 -1 -0.0393184 6 -0.9999965429 19 20 -1 0.187642 -1 0.029465 29 0.9999949932 22 23 -1 -0.0409084 -1 0.0715266 13 -0.4999999404 25 28 253 0.4451917708 26 27 -1 0.119437 -1 0.293898 -1 0.0638633 29 26 0.08896971494 1 18 304 0.02145929821 2 3 -1 -0.0666185 305 0.08072589338 4 7 302 0.5 5 6 -1 -0.0654297 -1 0.0135832 7 -0.403411299 8 15 278 -0.2447567284 9 12 314 12.5 10 11 -1 -0.0347078 -1 0.11329 191 0.05649028718 13 14 -1 0.183324 -1 0.071991 256 0.3393112719 16 17 -1 -0.042261 -1 0.0639286 305 0.08072589338 19 22 306 0.2075095177 20 21 -1 -0.0566416 -1 0.0488019 304 0.005086235236 23 24 -1 -0.0389845 278 -0.9101656675 25 26 -1 0.00547568 256 -0.1909158528 27 28 -1 -0.000169717 -1 0.066628 29 15 -0.2666666508"
   "1 20 268 -0.2666665912 2 9 315 0.97430408 3 8 279 -0.1074457467 4 5 -1 0.0574413 0 -0.6551013589 6 7 -1 -0.0420196 -1 0.0295744 -1 -0.061389 7 -0.3903642595 10 17 313 5.5 11 16 316 27 12 13 -1 -0.0964258 278 -0.590696156 14 15 -1 0.0247109 -1 -0.0628951 -1 -0.0897164 303 0.006296726409 18 19 -1 0.056847 -1 -0.0352371 26 0.1279449314 21 26 303 0.977835238 22 25 304 0.02145929821 23 24 -1 0.0745102 -1 0.0381115 -1 -0.0732311 256 0.07710429281 27 28 -1 0.0297574 -1 -0.0232324 29 315 0.9407407641 1 10 282 -1.567301035 2 9 316 77 3 8 276 -1.617049813 4 5 -1 0.165453 287 -1.379310131 6 7 -1 0.0967279 -1 -0.0690323 -1 -0.0782973 -1 -0.0806947 306 0.05012866482 11 16 246 0.01785713993 12 15 264 750000 13 14 -1 0.0924569 -1 -0.0592787 -1 -0.0533809 23 0.2575446665 17 20 315 0.9975062609 18 19 -1 -0.0683227 -1 0.0899841 13 -0.2499999702 21 26 240 -1.906950831 22 25 52 0.2857130468 23 24 -1 -0.00861064"
   "-1 0.146774 -1 0.00283465 77 0.4914687872 27 28 -1 -0.0619113 -1 0.0536698 29 304 0.07844910026 1 8 215 1.03619051 2 5 303 0.8358795643 3 4 -1 -0.0726657 -1 -0.0157846 207 0.7390460372 6 7 -1 0.17297 -1 -0.0696422 29 0.5714249015 9 24 8 -0.2611185312 10 21 258 -0.01807228662 11 18 277 -0.1236490384 12 15 297 -0.9166663885 13 14 -1 0.20174 -1 -0.0642711 73 -0.1283945292 16 17 -1 0.0769387 -1 0.00083616 17 0 19 20 -1 0.119617 -1 -0.0206276 93 -0.09544356912 22 23 -1 0.0294234 -1 -0.043234 202 -0.07692307234 25 26 -1 -0.0143586 239 -0.2813932896 27 28 -1 0.15623 -1 0.0807685 29 25 0.1232918203 1 20 304 0.01201449987 2 3 -1 -0.0679433 305 0.02718859538 4 5 -1 -0.0640096 313 5.5 6 9 254 -0.04112828523 7 8 -1 -0.049675 -1 0.00499302 250 125000 10 17 256 -0.7746472359 11 12 -1 -0.0222935 214 -0.2723002434 13 14 -1 0.0968953 254 0.1220607013 15 16 -1 -0.0525584 -1 0.0678516 1 -0.8640894294 18 19"
   "-1 0.48552 -1 0.0775564 312 2.5 21 28 38 0.4999999404 22 25 24 0.01550655067 23 24 -1 -0.0590888 -1 0.00112822 278 -0.590696156 26 27 -1 -0.00373295 -1 0.0574838 -1 0.0600863 29 303 0.002797129797 1 10 30 0.7262622714 2 9 122 -0.8152020574 3 8 53 -0.7063861489 4 5 -1 0.0604616 185 0.04274953902 6 7 -1 0.0070324 -1 -0.153467 -1 0.070982 -1 -0.0291457 38 0.3333332837 11 22 316 23 12 13 -1 -0.0639316 185 0.3080538213 14 19 314 12.5 15 16 -1 0.0446683 271 0.8124998808 17 18 -1 0.0123684 -1 -0.0787759 260 -0.8174664378 20 21 -1 0.0220323 -1 -0.0449711 313 5.5 23 28 316 27 24 25 -1 -0.0950118 191 0.4585247636 26 27 -1 0.00423389 -1 -0.0666721 -1 -0.0737659 29 315 0.9407407641 1 8 282 -1.567301035 2 7 316 77 3 6 269 0.2499997467 4 5 -1 0.111639 -1 -0.042237 -1 -0.0778545 -1 -0.0805193 306 0.05012866482 9 18 246 0.2499997467 10 17 226 250000 11 16 310 60 12 15 287 -1.180555463 13 14 -1 -0.13952 -1"
   "0.0520508 -1 0.089435 -1 -0.0300397 -1 -0.067929 23 0.2575446665 19 22 315 0.9975062609 20 21 -1 -0.067014 -1 0.0801229 13 -0.2499999702 23 28 126 0.4999997616 24 27 100 -0.0006512717227 25 26 -1 -0.00146401 -1 0.125799 -1 -0.0647115 -1 -0.0199361 29 304 0.06203655526 1 18 281 1.088909268 2 15 303 0.8358795643 3 4 -1 -0.0756919 5 -0.4806032479 5 8 246 0.09999991953 6 7 -1 -0.0696152 -1 0.121717 228 -0.4999999404 9 14 316 242 10 13 310 184 11 12 -1 0.0409207 -1 1.77787 -1 -0.0324043 -1 0.00330634 286 -0.5206947327 16 17 -1 -0.0729759 -1 0.222185 13 -0.4999999404 19 22 36 0.2499999702 20 21 -1 0.0349841 -1 0.1059 237 0.002467067679 23 26 28 0.6444302797 24 25 -1 -0.0310571 -1 0.0620274 260 0.331740886 27 28 -1 0.058524 -1 -0.0395212 29 26 0.06368613988 1 14 304 0.01748304255 2 3 -1 -0.0658534 305 0.02581990324 4 5 -1 -0.0626738 7 -0.403411299 6 11 278 -0.2447567284 7 8 -1 -0.0100817 304 0.3079019487"
   "9 10 -1 0.0983377 -1 0.0132785 256 0.1253453046 12 13 -1 -0.0537177 -1 0.0167027 305 0.2099691331 15 20 306 0.1093877777 16 17 -1 -0.0484041 313 5.5 18 19 -1 -0.00448191 -1 0.0641647 304 0.0048340098 21 22 -1 -0.0372059 254 0.03844545409 23 26 0 -0.6551013589 24 25 -1 0.0600113 -1 -0.031827 0 0.4236811101 27 28 -1 0.0621164 -1 0.0176936 29 303 0.002797129797 1 8 26 0.09683576971 2 3 -1 0.0695775 186 0.112651363 4 5 -1 0.0422236 311 1.5 6 7 -1 0.0378715 -1 -0.0817381 38 0.3333332837 9 22 315 0.9569377899 10 21 185 0.3080538213 11 18 253 0.04456672445 12 13 -1 0.0492028 313 4.5 14 15 -1 0.0262792 1 -0.1415985227 16 17 -1 -0.0621712 -1 0.0141941 260 -0.8174664378 19 20 -1 0.021407 -1 -0.0395831 -1 -0.0569236 313 5.5 23 28 316 27 24 25 -1 -0.0899377 191 0.4585247636 26 27 -1 0.00462108 -1 -0.060082 -1 -0.0670331 29 315 0.9407407641 1 8 282 -1.567301035 2 7 316 77 3 6 280 -1.082033634 4 5 -1 -0.0342644"
   "-1 0.114204 -1 -0.0774092 -1 -0.0803488 306 0.05012866482 9 18 246 0.2499997467 10 17 226 250000 11 16 310 60 12 15 260 0.002795569599 13 14 -1 -0.112056 -1 0.0579185 -1 0.084483 -1 -0.0319757 -1 -0.0663702 32 0.3611787558 19 26 36 0.3555555344 20 23 316 1 21 22 -1 0.0799677 -1 -0.0748379 49 0.5757212639 24 25 -1 -0.0124171 -1 0.166252 8 -1.12613368 27 28 -1 0.106878 -1 0.00999025 29 304 0.06203655526 1 20 212 1.15189445 2 19 303 0.8358795643 3 4 -1 -0.0744699 5 -0.4806032479 5 12 313 6.5 6 7 -1 -0.0629154 315 0.4147104919 8 11 307 15 9 10 -1 0.96905 -1 -0.0313849 -1 -0.0211771 228 -0.4374998808 13 18 247 -0.1249999553 14 15 -1 -0.039304 24 -0.003153520171 16 17 -1 -0.0204234 -1 0.929697 -1 -0.0261378 -1 0.0913217 28 0.2982421517 21 26 70 -0.2377953976 22 23 -1 0.0572251 17 -0.2857142389 24 25 -1 0.0254339 -1 -0.0386998 13 -0.2999999821 27 28 -1 0.100385 -1 0.0321324 29 25 0.1232918203 1"
   "14 313 5.5 2 9 189 -0.5574177504 3 4 -1 0.0250218 7 -0.770401299 5 8 255 -0.2086524665 6 7 -1 -0.0491704 -1 0.0438846 -1 -0.0623949 306 0.05138574541 10 11 -1 -0.0339046 240 -0.3905752897 12 13 -1 0.0841887 -1 0.00409624 255 -0.08699969202 15 18 15 -0.4999999702 16 17 -1 0.00785589 -1 -0.0538745 187 0.08069829643 19 28 304 0.0048340098 20 21 -1 -0.0516416 192 -0.4552225769 22 25 281 -0.4026041329 23 24 -1 0.0864181 -1 0.014229 312 1.5 26 27 -1 -0.0095669 -1 0.0347793 -1 0.0574868 29 15 -0.2666666508 1 20 268 -0.2666665912 2 9 6 -0.7142816186 3 6 308 15 4 5 -1 -0.0424314 -1 0.0194131 283 -0.3107375801 7 8 -1 0.0545434 -1 0.0125053 7 -0.3903642595 10 17 278 -0.09213772416 11 16 236 -0.9531190395 12 13 -1 -0.0832463 313 2.5 14 15 -1 0.0315082 -1 -0.0316135 -1 -0.0715153 260 0.5413770676 18 19 -1 0.0269972 -1 -0.0554425 26 0.09250487387 21 24 303 0.02213495411 22 23 -1 0.0661703 -1 0.0226841"
   "122 -0.9494446516 25 26 -1 -0.0508291 23 0.203529954 27 28 -1 0.0373624 -1 -0.0148609 29 315 0.9323583245 1 6 316 30 2 5 247 -0.3749999106 3 4 -1 0.338365 -1 -0.0596254 -1 -0.0794832 303 0.9334855676 7 16 78 0.3906914592 8 13 241 -1.349999785 9 12 174 -0.2857142389 10 11 -1 -0.0517871 -1 0.128971 -1 -0.0488221 244 0.0624999702 14 15 -1 0.0847899 -1 -0.0626381 307 20 17 20 125 4.232629299 18 19 -1 0.0873807 -1 -0.0607963 310 127 21 24 286 -5.146763802 22 23 -1 -0.178866 -1 -0.00579996 273 0.2678571045 25 28 216 -1.698880792 26 27 -1 -0.100128 -1 0.0928908 -1 -0.0796834 29 304 0.06203655526 1 18 281 1.088909268 2 15 303 0.8358795643 3 4 -1 -0.0743289 304 0.01431206148 5 6 -1 -0.0665659 297 -0.3749999106 7 14 231 -0.6441809535 8 9 -1 0.0165444 279 -0.4919701219 10 13 287 -0.3749999106 11 12 -1 0.857915 -1 -0.0324646 -1 0.0080852 -1 -0.0176944 286 -0.3804640174 16 17 -1 -0.0464838 -1 0.220616"
   "29 0.5714249015 19 26 17 -0.1481481194 20 25 279 -0.5633991361 21 22 -1 -0.0434955 238 -0.8513458967 23 24 -1 0.226678 -1 0.0383039 -1 -0.0217719 77 0.3913032413 27 28 -1 0.046829 -1 0.112892 29 26 0.06368613988 1 16 304 0.01748304255 2 3 -1 -0.0618425 305 0.02581990324 4 5 -1 -0.0593527 15 -0.4999999702 6 9 268 -0.6136363149 7 8 -1 -0.0183085 -1 0.0570596 255 0.07633252442 10 11 -1 -0.0392282 253 0.1349620521 12 13 -1 -0.0297891 308 2 14 15 -1 0.311932 -1 0.0499759 305 0.2099691331 17 20 306 0.1093877777 18 19 -1 -0.0456843 -1 0.0194184 304 0.0048340098 21 22 -1 -0.0330677 308 200 23 28 283 -1.040291667 24 25 -1 -0.0193536 256 0.03019007109 26 27 -1 0.015344 -1 0.0527038 -1 -0.0245062 29 303 0.002797129797 1 12 48 0.1671199799 2 3 -1 0.0655287 185 -0.0008678102749 4 5 -1 0.0407102 186 0.08697069436 6 7 -1 0.0333876 311 1.5 8 9 -1 0.0114449 130 -0.6666665673 10 11 -1 -0.235639 -1 -0.0689075"
   "309 128 13 20 26 0.06112449616 14 17 145 0.4225391448 15 16 -1 0.039672 -1 -0.0256992 279 -1.00042367 18 19 -1 0.0298163 -1 -0.0284589 240 -1.55513525 21 22 -1 -0.0588849 184 0.294996798 23 28 313 5.5 24 27 253 0.2768595815 25 26 -1 0.0452029 -1 -0.00708335 -1 -0.0285121 -1 -0.0524591 29 315 0.9323583245 1 6 316 30 2 5 247 -0.3749999106 3 4 -1 0.292203 -1 -0.05775 -1 -0.0791731 303 0.9334855676 7 18 78 0.3906914592 8 15 241 -1.145833135 9 12 197 -0.1999999583 10 11 -1 -0.0474579 -1 0.117867 29 0.3749983609 13 14 -1 -0.074783 -1 -0.00665247 244 0.0624999702 16 17 -1 0.0772291 -1 -0.0605405 307 20 19 20 -1 0.0820266 310 127 21 24 286 -5.146763802 22 23 -1 -0.163285 -1 -0.00663379 273 0.1071428433 25 28 216 -1.698880792 26 27 -1 -0.0958957 -1 0.0928977 -1 -0.0516757 29 304 0.06203655526 1 18 281 1.088909268 2 15 303 0.8358795643 3 4 -1 -0.0735585 305 0.04395278543 5 12 234 -0.4737493694 6 7"
   "-1 -0.0549907 316 208 8 11 315 0.6108843684 9 10 -1 0.391836 -1 -0.0551066 -1 -0.0420661 296 -0.2999999523 13 14 -1 0.439315 -1 -0.0486461 286 -0.5206947327 16 17 -1 -0.0707238 -1 0.182932 28 0.4382185638 19 24 70 -0.292842567 20 21 -1 0.0542249 17 -0.2857142389 22 23 -1 0.0260971 -1 -0.0305185 253 0.4508504272 25 26 -1 0.0566621 217 -0.3054455221 27 28 -1 0.20791 -1 -0.0182126 29 25 0.1010709852 1 20 306 0.02685395442 2 3 -1 -0.0604785 7 -0.770401299 4 17 256 -0.9346600771 5 8 276 -2.64773798 6 7 -1 0.352113 -1 -0.0313077 270 -0.5999999046 9 14 238 -0.2998947203 10 13 308 26 11 12 -1 1.14761 -1 -0.029417 -1 0.162836 282 -1.499994993 15 16 -1 0.253633 -1 0.0473441 254 0.1352441758 18 19 -1 -0.0498977 -1 0.0216489 312 2.5 21 28 255 -0.06976646185 22 23 -1 -0.0361609 278 -0.6997478604 24 25 -1 -0.0127165 7 -0.3903642595 26 27 -1 0.0526256 -1 -0.000105668 -1 0.0410038 29 15 -0.1999999881 1 24"
   "268 -0.1249999702 2 13 316 208 3 8 310 119 4 5 -1 0.0368527 305 0.002824444789 6 7 -1 -0.086124 -1 -0.0161121 279 -0.04492748529 9 10 -1 0.0561953 0 -0.5036461949 11 12 -1 -0.0278928 -1 0.0363354 7 -0.403411299 14 21 278 -0.09806717187 15 20 236 -0.9305508733 16 17 -1 -0.0754775 313 2.5 18 19 -1 0.0297566 -1 -0.0266173 -1 -0.064173 303 0.006296726409 22 23 -1 0.041302 -1 -0.0278637 26 0.09250487387 25 26 -1 0.0544877 122 -0.900801897 27 28 -1 -0.0413145 -1 0.0139389 29 315 0.9198217988 1 4 316 27 2 3 -1 0.125659 -1 -0.0795828 123 0.004351800773 5 28 100 -0.003229864873 6 11 256 0.5686151385 7 10 303 0.9495077133 8 9 -1 -0.059819 -1 0.0310103 -1 0.0915513 97 -0.001370203681 12 13 -1 -0.0444376 151 -0.5555555224 14 15 -1 -0.0294851 259 -1.921865344 16 23 310 127 17 18 -1 -0.124859 9 -2.03040576 19 20 -1 -0.0915401 273 0.0624999702 21 22 -1 0.0826769 -1 -0.0550729 270 125000 24 27 282 0.1718741357"
   "25 26 -1 0.0826803 -1 -0.0827797 -1 -0.0714031 -1 -0.0478111 29 303 0.02213495411 1 8 138 1.379721284 2 5 208 1.007637382 3 4 -1 -0.0672352 -1 0.0806372 8 -0.4922322929 6 7 -1 0.268941 -1 -0.036179 277 -0.6820201278 9 10 -1 -0.0500998 13 -0.4999999404 11 18 305 0.002824444789 12 15 295 -1.57499969 13 14 -1 0.617251 -1 0.123205 28 0.4109366536 16 17 -1 0.0283406 -1 0.0950158 316 584 19 28 6 -0.7142816186 20 21 -1 0.0713766 269 1125000 22 25 279 0.1085681692 23 24 -1 -0.0325903 -1 0.0253423 252 0.5833331347 26 27 -1 0.0189676 -1 0.637542 -1 -0.0778692 29 26 0.03571890667 1 16 304 0.01748304255 2 3 -1 -0.0598736 305 0.02581990324 4 11 316 728 5 6 -1 -0.0640262 273 0.0624999851 7 8 -1 -0.027463 223 -0.312499851 9 10 -1 0.698557 -1 0.0532853 313 5.5 12 15 256 -0.03770460188 13 14 -1 -0.0418024 -1 0.0157831 -1 0.0367508 313 5.5 17 24 254 -0.00567881763 18 19 -1 -0.0335269 316 269 20 23 193 -0.7738231421"
   "21 22 -1 0.0821595 -1 0.022126 -1 -0.012587 191 -0.682463944 25 26 -1 -0.0146784 304 0.5385776758 27 28 -1 0.0464891 -1 -0.0370334 29 303 0.003647685051 1 10 26 0.06628225744 2 3 -1 0.0624004 186 0.09052088112 4 5 -1 0.0375096 311 1.5 6 7 -1 0.0339053 130 -0.8571428061 8 9 -1 -0.183838 -1 -0.0448201 256 -0.1826754957 11 22 6 -0.7142816186 12 17 308 16 13 16 307 11 14 15 -1 -0.0839186 -1 -0.0217564 -1 0.0151306 185 0.3938509524 18 19 -1 0.039454 268 -0.8124998212 20 21 -1 0.0529081 -1 -0.0153296 184 0.7272695303 23 28 280 -0.8924732208 24 25 -1 -0.0605409 313 5.5 26 27 -1 0.00803592 -1 -0.0324953 -1 -0.0606984 29 315 0.9198217988 1 6 316 30 2 5 259 0.5673055053 3 4 -1 -0.0633388 -1 0.251497 -1 -0.0794704 115 0.00666721072 7 28 115 -0.003510124749 8 21 92 0.208831504 9 18 253 0.3180282116 10 15 29 1.142850637 11 12 -1 -0.0635632 267 0.02083333023 13 14 -1 0.0993708 -1 -0.0556389 222 -0.07954544574"
   "16 17 -1 0.135008 -1 -0.0155056 97 0.01489770319 19 20 -1 -0.0820951 -1 0.0764763 259 -1.921865344 22 27 310 127 23 24 -1 -0.11292 14 -0.1666666418 25 26 -1 0.0781159 -1 -0.0509727 -1 0.0796076 -1 -0.0453974 29 304 0.06203655526 1 14 281 1.088909268 2 13 303 0.8358795643 3 4 -1 -0.0719876 304 0.01431206148 5 6 -1 -0.0631445 312 2.5 7 10 316 215 8 9 -1 0.134639 -1 -0.0701505 305 0.0197160095 11 12 -1 0.0802932 -1 0.539496 -1 0.0882848 29 0.5714249015 15 26 8 -0.2297101617 16 25 258 -0.01807228662 17 24 297 -0.9166663885 18 21 279 -0.5633991361 19 20 -1 -0.05448 -1 0.379724 276 0.1189935133 22 23 -1 -0.0410993 -1 0.0252755 -1 0.0724654 -1 -0.0218341 77 0.4207175076 27 28 -1 0.0377525 -1 0.0968984 29 304 0.004999093246 1 2 -1 -0.0548822 305 0.00966856163 3 4 -1 -0.0694163 15 -0.6153845787 5 18 304 0.6852914691 6 17 31 1.072275758 7 16 314 4.5 8 11 278 -0.9332165122 9 10 -1 -0.0593459 -1 0.0320827"
   "268 -0.9999998808 12 13 -1 -0.0398866 270 -0.5999999046 14 15 -1 0.16971 -1 0.0513347 -1 -0.0308934 -1 -0.0548646 256 0.1253453046 19 24 275 1.949999571 20 21 -1 -0.0276907 230 -0.6079650521 22 23 -1 0.386419 -1 -0.0174697 308 68 25 28 212 -0.05800274014 26 27 -1 -0.00231363 -1 0.0504285 -1 -0.0142694 29 7 -0.242622897 1 18 260 -0.0510475561 2 11 0 -0.2928439975 3 10 268 -0.6136363149 4 5 -1 0.0145681 240 -0.377468884 6 7 -1 -0.0470609 279 -0.1317126155 8 9 -1 0.0479015 -1 -0.0432975 -1 0.0388291 313 5.5 12 17 316 27 13 14 -1 -0.0759375 278 -0.6518939734 15 16 -1 0.0207139 -1 -0.0303431 -1 -0.0533329 26 0.1218636259 19 24 308 2 20 21 -1 -0.0574151 187 0.08838365972 22 23 -1 0.0588628 -1 0.014522 254 0.1092033908 25 26 -1 0.031249 316 51 27 28 -1 -0.0738672 -1 -0.00584564 29 315 0.9198217988 1 6 316 30 2 5 259 0.5673055053 3 4 -1 -0.0621393 -1 0.222427 -1 -0.0792615 306 0.05012866482 7 22"
   "247 0 8 21 257 -1.034695029 9 18 310 127 10 13 286 -5.146763802 11 12 -1 -0.138899 -1 0.0501051 304 0.004671248142 14 15 -1 0.0727971 310 199 16 17 -1 0.0918506 -1 -0.097027 270 125000 19 20 -1 0.0732116 -1 -0.0889441 -1 -0.0594536 23 0.2575446665 23 24 -1 -0.0569807 13 -0.2499999702 25 28 240 -1.906950831 26 27 -1 0.0881463 -1 -0.0140141 -1 -0.021928 29 303 0.02213495411 1 8 138 1.379721284 2 5 231 1.001167417 3 4 -1 -0.0645391 -1 0.0805109 8 -0.4922322929 6 7 -1 0.213303 -1 -0.033467 277 -0.6936935782 9 12 270 3.999995947 10 11 -1 -0.0565199 -1 0.162368 21 -0.4999999106 13 24 227 -0.2083332688 14 15 -1 -0.00432406 305 0.002824444789 16 21 316 215 17 20 308 8 18 19 -1 0.130947 -1 0.441327 -1 0.0672888 267 0.3437499106 22 23 -1 0.0318845 -1 0.107774 316 530 25 28 38 0 26 27 -1 0.0018683 -1 0.0541809 -1 -0.0457534 29 304 0.004999093246 1 2 -1 -0.0522516 305 0.00966856163 3 4 -1 -0.0680799"
   "264 -0.3999999166 5 6 -1 -0.0399013 15 -0.6153845787 7 22 31 1.072275758 8 21 270 -0.6874998212 9 14 310 224 10 11 -1 -0.0417451 228 -0.2031249702 12 13 -1 0.00480173 -1 0.291538 236 -1.482137561 15 16 -1 0.108898 278 -0.796205461 17 20 284 -0.711784482 18 19 -1 0.0610636 -1 -0.022275 -1 0.0450622 -1 -0.0281494 256 0.1023950949 23 26 275 1.949999571 24 25 -1 -0.0216628 -1 0.14033 308 68 27 28 -1 0.0338756 -1 -0.008522 29 303 0.006296726409 1 10 186 0.1681718379 2 5 23 0.3153951466 3 4 -1 0.0598018 -1 0.00551503 311 1.5 6 7 -1 0.0326852 185 -0.004002004862 8 9 -1 0.0148021 -1 -0.0706074 309 64 11 16 309 52 12 15 234 0.2787982523 13 14 -1 -0.00938769 -1 0.0264962 -1 0.0742279 268 -0.3513513207 17 20 284 -1.18479526 18 19 -1 -0.04451 -1 0.0253733 240 -2.101703882 21 22 -1 -0.0638126 313 5.5 23 28 306 0.2075095177 24 27 305 0.08072589338 25 26 -1 0.0409344 -1 -0.0314984 -1 0.028805 -1 -0.0428079"
   "29 315 0.9198217988 1 4 316 30 2 3 -1 0.083824 -1 -0.0789346 123 0.004351800773 5 28 123 -0.001339851297 6 17 97 0.04842729121 7 10 303 0.9909460545 8 9 -1 -0.0584546 -1 0.0978588 290 -0.7499998212 11 12 -1 0.122393 100 0.1369693279 13 14 -1 -0.0207412 163 -0.000151631466 15 16 -1 0.0739529 -1 -0.0362805 259 -1.921865344 18 21 310 127 19 20 -1 -0.0970626 -1 0.0507077 259 0.4423059821 22 23 -1 0.0809587 286 -2.916622877 24 27 268 -0.6249999404 25 26 -1 -0.12783 -1 0.0687914 -1 -0.183407 -1 -0.0441894 29 304 0.0402447246 1 10 281 1.088909268 2 9 303 0.8518303037 3 4 -1 -0.0741285 212 -0.366833061 5 6 -1 -0.0489164 266 0.1874999702 7 8 -1 0.188497 -1 -0.0559131 -1 0.0946869 36 0.3999999166 11 24 17 0.1052631363 12 23 316 264 13 20 303 0.8921467066 14 17 179 -0.09090908617 15 16 -1 -0.033034 -1 0.0500485 251 -0.5499998927 18 19 -1 0.480877 -1 -0.0480306 279 -0.2782645822 21 22 -1 -0.0651227 -1"
   "0.00236814 -1 -0.0467508 11 -0.4999999404 25 26 -1 0.0881738 37 1.249999642 27 28 -1 0.0111677 -1 0.10694 29 305 0.008730986156 1 2 -1 -0.063632 304 0.004999093246 3 4 -1 -0.0499583 268 -0.08928570151 5 16 7 -0.7238553762 6 13 274 -0.3333332837 7 8 -1 -0.0717282 315 0.183116883 9 12 246 -0.3749998212 10 11 -1 0.467893 -1 0.0658777 -1 0.0116552 313 6.5 14 15 -1 -0.0601679 -1 0.00610438 15 -0.3999999464 17 24 28 0.6154760122 18 23 278 -0.2110537142 19 22 284 -0.7742184997 20 21 -1 0.0757331 -1 0.00418397 -1 0.0517692 -1 -0.0132525 254 0.1220607013 25 26 -1 -0.023272 310 63 27 28 -1 0.039015 -1 0.000853187 29 303 0.009172229096 1 16 184 0.3935632706 2 13 122 -0.8152020574 3 4 -1 -0.016665 30 0.5160998106 5 10 248 -0.9999989867 6 9 243 0.02499999665 7 8 -1 0.0474962 -1 -0.670483 -1 0.0603618 308 83 11 12 -1 -0.0261203 -1 0.0556676 199 0.3333332837 14 15 -1 0.0107644 -1 -0.0665336 305 0.5063171983"
   "17 28 315 0.9605399966 18 27 278 -0.009887663648 19 24 284 -0.9281486273 20 23 8 -0.3305120468 21 22 -1 -0.000389058 -1 -0.134765 -1 0.0273057 6 0.3499985933 25 26 -1 -0.023012 -1 0.0212259 -1 -0.0428398 -1 -0.0390984 29 315 0.9198217988 1 6 316 30 2 3 -1 0.0788018 303 0.9888951182 4 5 -1 -0.0800271 -1 0.0516357 123 0.004351800773 7 28 123 -0.001339851297 8 15 97 0.04842729121 9 10 -1 -0.0463974 290 -0.6666664481 11 12 -1 0.100235 260 0.1251332164 13 14 -1 -0.0343324 -1 0.0428357 259 -1.921865344 16 23 315 0.97430408 17 18 -1 -0.0866038 304 0.004372295924 19 20 -1 0.0726089 8 -1.274243832 21 22 -1 0.0406766 -1 -0.160748 259 0.4423059821 24 25 -1 0.078372 286 -2.916622877 26 27 -1 0.0415221 -1 -0.162855 -1 -0.0420471 29 303 0.02213495411 1 6 211 0.8809847236 2 5 192 1.714220762 3 4 -1 -0.0625292 -1 0.17229 -1 0.036085 277 -0.6936935782 7 8 -1 -0.0438958 29 0.714282155 9 28 316 355 10 21 5"
   "0.08301381022 11 20 251 -0.4999999106 12 15 272 -1.099999905 13 14 -1 0.350291 -1 0.0799369 187 0.07682452351 16 17 -1 -0.0283358 222 -0.2159090787 18 19 -1 0.0325631 -1 0.111302 -1 -0.0167149 246 1375000 22 23 -1 -0.0189205 244 0.4374997914 24 25 -1 -0.0553961 260 0.04159367457 26 27 -1 0.0260925 -1 0.713375 -1 0.0605824 29 305 0.008730986156 1 2 -1 -0.0619325 304 0.004999093246 3 6 189 1.519405842 4 5 -1 -0.0486402 -1 0.419256 264 -0.3999999166 7 8 -1 -0.0386607 313 5.5 9 22 188 -0.5351531506 10 11 -1 0.0397874 199 0.6666664481 12 21 224 -0.624999404 13 18 279 -1.019223094 14 15 -1 -0.0714764 7 -0.7648393512 16 17 -1 0.252537 -1 -0.0562078 268 -0.1249999851 19 20 -1 -0.0498641 -1 -0.00743127 -1 0.0289245 304 0.5136743188 23 28 0 -0.1484478116 24 25 -1 0.0449406 258 -0.04953175783 26 27 -1 -0.0307606 -1 0.0269253 -1 -0.0242519 29 0 -0.1641102433 1 20 253 -0.1897844076 2 13 7 -0.008533076383"
   "3 12 6 -0.7142816186 4 9 253 -0.8361175656 5 8 307 15 6 7 -1 -0.0491755 -1 0.0228023 -1 -0.0476118 278 0.1386536956 10 11 -1 0.0264915 -1 -0.019814 -1 0.0562799 313 5.5 14 19 315 0.9641909599 15 18 191 0.3018593192 16 17 -1 0.00691524 -1 -0.0409249 -1 -0.076967 -1 -0.0493864 254 0.1309258938 21 22 -1 0.0487812 304 0.01201449987 23 26 258 1.155133605 24 25 -1 0.0461305 -1 -0.09048 255 -0.08699969202 27 28 -1 0.0324017 -1 -0.0200465 29 315 0.9198217988 1 6 296 -0.4999998808 2 5 316 66 3 4 -1 0.0882196 -1 -0.0737057 -1 -0.080082 306 0.05012866482 7 16 310 59 8 9 -1 -0.048221 263 -2777.777832 10 11 -1 -0.047722 286 -10.1536808 12 15 285 -2.091616631 13 14 -1 -0.0480318 -1 0.083834 -1 0.0426034 23 0.1976930797 17 18 -1 -0.056312 290 -0.9999989867 19 20 -1 0.126132 159 -0.4999998808 21 22 -1 -0.0749498 146 0 23 28 146 -0.002963569714 24 27 195 -0.1666666418 25 26 -1 0.0649436 -1 -0.0507757 -1 0.10944"
   "-1 -0.0495802 29 304 0.01431206148 1 4 235 1.148633599 2 3 -1 -0.0740042 -1 0.117348 13 -0.4999999404 5 18 34 0.4999998808 6 13 268 -0.8365383744 7 10 265 0.6874996424 8 9 -1 -0.0378138 -1 0.0852302 226 -0.2083332986 11 12 -1 0.0645373 -1 -0.00693166 173 -0.3999999166 14 15 -1 0.0151014 304 0.1783388108 16 17 -1 0.185056 -1 0.0826878 14 -0.1249999851 19 24 267 0.4374997914 20 21 -1 0.000799398 255 -0.8933653235 22 23 -1 -0.0433771 -1 0.102533 201 0.5925924778 25 26 -1 -0.0395105 255 1.211292386 27 28 -1 0.00815967 -1 0.383679 29 305 0.007999050431 1 2 -1 -0.0623088 304 0.004999093246 3 8 146 0.7978945971 4 7 7 -0.6847823858 5 6 -1 0.0317906 -1 -0.0600652 -1 0.0992622 220 -0.2249999493 9 12 314 11.5 10 11 -1 -0.035268 -1 0.0107795 280 -0.6740304232 13 14 -1 0.044395 278 -0.02950038947 15 20 289 -0.2187499404 16 19 303 0.006647820119 17 18 -1 -0.060067 -1 0.0493769 -1 -0.0154855 304 0.5136743188"
   "21 28 2 0.2303120047 22 25 248 -0.7692307234 23 24 -1 0.404795 -1 0.0363311 187 0.05810463056 26 27 -1 -0.0414188 -1 0.0367746 -1 -0.0297216 29 7 -0.242622897 1 14 255 -0.1241060197 2 9 236 -0.484372586 3 4 -1 -0.0175207 268 -0.8124998212 5 6 -1 0.0503094 0 -0.56877774 7 8 -1 -0.0141424 -1 0.0308908 184 0.5545879006 10 13 284 -0.9740988612 11 12 -1 -0.0609423 -1 -0.0106115 -1 -0.0509154 130 -0.8571428061 15 24 61 -0.7499998212 16 17 -1 0.0161963 94 0.1956354976 18 21 122 -0.614297986 19 20 -1 -0.171376 -1 0.0105966 311 0.5 22 23 -1 -0.130519 -1 0.0289974 23 0.3344944417 25 28 308 3 26 27 -1 -0.0131782 -1 0.0468784 -1 -0.00810891 29 315 0.9198217988 1 4 316 30 2 3 -1 0.072143 -1 -0.0783585 169 0 5 28 169 -0.008797541261 6 19 253 0.3875407875 7 16 240 -2.101703882 8 15 14 -0.4999997616 9 10 -1 0.0484337 308 20 11 14 92 0.208831504 12 13 -1 -0.0752032 -1 0.0235495 -1 0.0924824 -1 -0.0734843"
   "310 60 17 18 -1 -0.061518 -1 0.0853908 212 -1.203808308 20 27 8 -1.179260135 21 22 -1 0.0570647 307 23 23 24 -1 0.0360049 307 59 25 26 -1 -0.167728 -1 0.062709 -1 0.0773507 -1 -0.0479983 29 303 0.01638398133 1 6 281 1.088909268 2 5 115 1.181814313 3 4 -1 -0.0639222 -1 0.0916226 -1 0.0665838 277 -0.6936935782 7 12 313 6.5 8 9 -1 -0.061261 59 0.9999998212 10 11 -1 0.0103491 -1 0.287974 37 0.7142856121 13 26 33 2.899915934 14 25 316 504 15 24 243 -1.124999762 16 19 266 -1.666666389 17 18 -1 -0.0627084 -1 0.162405 6 0.3636350334 20 23 56 -1.741919756 21 22 -1 0.0828414 -1 0.0132736 -1 -0.0240147 -1 -0.0420396 -1 0.0651426 28 0.151925087 27 28 -1 0.00539288 -1 0.0882255 29 305 0.007999050431 1 2 -1 -0.0606681 304 0.004999093246 3 8 313 6.5 4 5 -1 -0.0547752 185 0.03756197169 6 7 -1 -0.0367053 -1 0.103425 264 -0.3999999166 9 10 -1 -0.0365577 304 0.6852914691 11 28 313 5.5 12 25 228 -0.3035713732"
   "13 14 -1 -0.0501573 224 -0.624999404 15 18 310 217 16 17 -1 -0.0283849 -1 0.207644 316 249 19 24 193 -0.3846663535 20 23 308 73 21 22 -1 0.0667061 -1 -0.0200184 -1 0.00815047 -1 -0.0116182 275 1.749999166 26 27 -1 0.0245751 -1 0.11747 -1 -0.0487503 29 303 0.009172229096 1 14 187 0.08838365972 2 7 71 0.1249144599 3 6 248 -0.9999989867 4 5 -1 -0.310711 -1 0.0568914 -1 0.0139507 308 133 8 13 138 -0.2575495541 9 10 -1 0.0282766 122 -0.8152020574 11 12 -1 -0.121246 -1 -0.0282179 -1 0.0376398 305 0.5063171983 15 28 284 -1.00289166 16 19 315 0.3992395401 17 18 -1 0.0598889 -1 -0.0466582 278 -0.009887663648 20 25 315 0.9855421782 21 24 296 6.124993801 22 23 -1 0.0243381 -1 -0.0718211 -1 -0.0436266 16 0 26 27 -1 -0.0174172 -1 0.0284213 -1 -0.0323376 29 316 66 1 28 247 0 2 27 172 0.1666666418 3 26 197 -0.1777777672 4 9 213 0.3124976456 5 6 -1 -0.0366963 222 -0.4999999702 7 8 -1 -0.0889311 -1 0.132224"
   "287 -1.312499642 10 13 32 -0.04816072434 11 12 -1 0.256774 -1 0.0699033 287 -1.249999642 14 17 252 -0.3999999166 15 16 -1 -0.151228 -1 0.0505387 77 -0.00202404242 18 21 254 0.1265521497 19 20 -1 -0.0653537 -1 0.0914372 220 -2.343749523 22 23 -1 -0.21606 170 -0.004883887712 24 25 -1 0.0142529 -1 0.0634032 -1 -0.0569739 -1 -0.0701493 -1 -0.0793079 29 17 0.1052631363 1 28 304 0.01431206148 2 5 215 0.9024963379 3 4 -1 -0.0741466 -1 0.117341 179 -0.09090908617 6 7 -1 -0.028492 38 0.1333333105 8 19 251 -0.5499998927 9 14 222 -0.9531248808 10 11 -1 -0.0743877 306 0.08501250297 12 13 -1 0.147587 -1 -0.0489116 6 -0.1111106202 15 18 259 -0.4044109285 16 17 -1 -0.0154046 -1 0.0470102 -1 -0.0197657 303 0.1924695075 20 25 277 0.04340372235 21 22 -1 -0.0388748 270 1.624998331 23 24 -1 0.0411193 -1 0.195629 167 0.599997282 26 27 -1 0.0804075 -1 0.207802 -1 -0.0395206 29 268 -0.08928570151 1 8 306 0.02532918379"
   "2 3 -1 -0.0584661 7 -0.7498456836 4 5 -1 0.0158717 313 6.5 6 7 -1 -0.0572046 -1 0.0225956 15 -0.6153845787 9 12 28 0.3760760128 10 11 -1 0.0364625 -1 -0.00590615 310 63 13 18 308 72 14 15 -1 0.0335971 130 -0.6249999404 16 17 -1 0.0545149 -1 -0.0316999 218 -0.2249999493 19 20 -1 -0.0598821 304 0.008792823181 21 28 226 500000 22 25 130 -0.9999999404 23 24 -1 0.0817486 -1 -0.0572854 248 125000 26 27 -1 0.068512 -1 0.61237 -1 0.0015081 29 0 -0.1566521376 1 20 254 -0.6002033949 2 7 227 9.249990463 3 6 34 0.5999999046 4 5 -1 0.0214151 -1 -0.0215279 -1 -0.0886126 313 6.5 8 19 316 14 9 10 -1 -0.0661812 276 0.2233695388 11 18 314 12.5 12 15 197 -0.9999998808 13 14 -1 -0.0435446 -1 0.0214498 245 -0.4166666269 16 17 -1 -0.0727956 -1 0.0232599 -1 -0.0374258 -1 -0.045315 254 0.1309258938 21 22 -1 0.0435064 255 0.04332107306 23 26 1 0.04470402375 24 25 -1 -0.00803065 -1 0.0437666 188 -0.6662897468 27 28"
   "-1 -0.0563475 -1 -0.00204335 29 316 66 1 28 244 0.01562499814 2 27 115 0.01513232198 3 26 115 -0.005400279071 4 13 253 0.4451917708 5 12 194 -3.444432497 6 9 13 -0.1666666418 7 8 -1 0.0483108 -1 -0.0478582 284 -1.057846546 10 11 -1 0.00246146 -1 -0.097178 -1 0.0797317 212 -1.203808308 14 25 8 -1.179260135 15 18 266 -1.395833135 16 17 -1 -0.0184174 -1 0.065315 235 -1.521043301 19 22 222 -0.4583332837 20 21 -1 -0.329027 -1 0.000310473 306 0.004831784405 23 24 -1 0.0726065 -1 -0.0936549 -1 0.0715402 -1 -0.0295623 -1 -0.066429 -1 -0.0791492 29 13 -0.4999999404 1 10 267 0.3928570747 2 7 59 1.66666615 3 6 257 0.9218785763 4 5 -1 -0.00208082 -1 0.104735 -1 0.090022 6 -0.1111106798 8 9 -1 0.0981757 -1 0.0250456 72 -0.2621124983 11 12 -1 0.0332138 13 3.999998569 13 26 258 0.8880009651 14 19 17 -0.2857142389 15 18 260 -0.6444851756 16 17 -1 -0.0601365 -1 0.0167663 -1 -0.0480218 5 0.3728868961 20 25"
   "315 0.7826086879 21 24 306 0.02078430541 22 23 -1 0.0205381 -1 0.296945 -1 -0.0939578 -1 -0.000171464 223 2.374997616 27 28 -1 0.0226807 -1 0.451302 29 305 0.007999050431 1 2 -1 -0.0576773 304 0.004754298832 3 6 146 0.7978945971 4 5 -1 -0.0453958 -1 0.102866 220 -0.2249999493 7 8 -1 -0.0178428 280 -0.6740304232 9 10 -1 0.0367769 279 -0.8279595971 11 14 240 -2.165094156e-05 12 13 -1 -0.00584059 -1 -0.0686779 294 -0.7777776718 15 16 -1 0.409619 300 0.5 17 24 308 101 18 19 -1 0.0241098 1 -0.09384018928 20 23 225 0.4749999046 21 22 -1 -0.00928913 -1 0.0541078 -1 -0.0401814 316 575 25 26 -1 -0.0544583 242 -0.1562499553 27 28 -1 0.357358 -1 0.0270845 29 38 0.3333332837 1 22 303 0.01043619774 2 11 122 -0.8557007313 3 8 145 -0.7031280994 4 5 -1 0.0149878 94 -0.1274059117 6 7 -1 0.026174 -1 -0.149524 146 1.212288737 9 10 -1 0.0445809 -1 -0.117928 315 0.7151162624 12 21 223 2.999996901 13 14 -1 0.0143583"
   "278 -3.951988459 15 20 316 404 16 19 316 340 17 18 -1 0.0363961 -1 -0.656356 -1 0.0394945 -1 -0.0241871 -1 -0.0198779 168 0.8689664006 23 28 296 -0.2916665673 24 25 -1 -0.0508445 312 1.5 26 27 -1 0.0170161 -1 -0.0180133 -1 -0.0455896 29 316 66 1 28 247 0 2 27 126 0.4444443882 3 26 100 -0.003229864873 4 7 257 -0.1356904209 5 6 -1 -0.0482618 -1 0.0597107 120 0.001579230884 8 25 4 -0.9813854098 9 24 8 -1.014009237 10 13 307 14 11 12 -1 0.0691735 -1 0.0154566 22 -0.4999998808 14 23 17 -0.4999998808 15 16 -1 0.0680691 15 -0.2142857015 17 22 207 -1.070219159 18 19 -1 -0.0295112 101 0.01975769363 20 21 -1 -0.500667 -1 -0.139266 -1 0.0477369 -1 0.0187626 -1 0.0787543 -1 -0.0436096 -1 -0.0545387 -1 -0.0678939 -1 -0.0789457 29 304 0.01431206148 1 4 208 1.007637382 2 3 -1 -0.0717591 -1 0.102482 201 -0.111111097 5 6 -1 -0.0336034 37 0.7142856121 7 24 316 358 8 21 156 -0.1176470444 9 10 -1 -0.0363875"
   "251 -0.2708332837 11 14 303 0.8664807081 12 13 -1 0.0521018 -1 0.184043 71 -0.2143361717 15 16 -1 0.0698641 25 0.085648112 17 18 -1 -0.0190207 303 0.04495660216 19 20 -1 -0.0371323 -1 0.0700751 21 -1.333333135 22 23 -1 0.101796 -1 -0.0224434 46 0.5121764541 25 26 -1 0.0251673 275 0.2187499702 27 28 -1 0.0190408 -1 0.132817 29 264 -0.4642856419 1 2 -1 -0.0489729 313 6.5 3 26 7 -0.7238553762 4 19 303 0.6158702374 5 12 224 -0.624999404 6 11 279 -1.052762866 7 8 -1 -0.0462925 273 0.2291666269 9 10 -1 0.220722 -1 -0.0166555 -1 0.0200026 291 -2.593749285 13 18 5 -1.626220822 14 17 7 -3.500559807 15 16 -1 -0.0513562 -1 0.470101 -1 0.0144231 -1 -0.0497536 268 0.2083333135 20 23 224 1000000 21 22 -1 -0.0369727 -1 0.156695 304 0.009194826707 24 25 -1 -0.0407156 -1 0.0168241 304 0.1499272436 27 28 -1 0.0343567 -1 -0.00106907 29 0 -0.1566521376 1 24 305 0.5063171983 2 21 315 0.9802469015 3 20 279 -0.09949540347"
   "4 17 314 6.5 5 8 316 56 6 7 -1 -0.00476831 -1 0.0555002 232 -6.124491215 9 14 315 0.6229050159 10 11 -1 0.0785745 315 0.67469877 12 13 -1 -0.651645 -1 0.0180684 314 14.5 15 16 -1 0.00992684 -1 -0.0530771 236 -0.1874987483 18 19 -1 -0.0326687 -1 0.0025422 -1 -0.0372479 271 0.6249998212 22 23 -1 -0.0123004 -1 -0.0552365 254 0.1309258938 25 26 -1 0.0397497 255 0.04332107306 27 28 -1 0.0239192 -1 -0.0101793 29 316 66 1 28 244 0.01562499814 2 27 294 0 3 26 11 -1.333333135 4 11 275 0.2499997467 5 6 -1 0.0778735 280 -1.002052188 7 10 266 0.6499998569 8 9 -1 0.0676895 -1 -0.170957 -1 -0.212576 11 -1.249999642 12 15 305 0.001603788231 13 14 -1 0.0605162 -1 -0.125316 253 0.3875407875 16 25 170 -0.02522555925 17 22 247 -0.2083332688 18 19 -1 -0.00379658 229 0.624999702 20 21 -1 -0.0990993 -1 0.112265 120 0.004357781727 23 24 -1 0.0458847 -1 -0.0672838 -1 0.0786675 -1 -0.0801994 -1 -0.0639267 -1 -0.0787559"
   "29 17 0.1052631363 1 22 306 0.4056941271 2 21 16 0 3 20 83 1.199999809 4 17 268 -0.8365383744 5 6 -1 -0.0155366 167 0.4166652858 7 16 13 -0.1999999583 8 15 255 -0.4510059953 9 12 251 -0.5156249404 10 11 -1 0.121126 -1 0.038204 289 -0.08333330601 13 14 -1 -0.0497138 -1 0.0340162 -1 -0.0161123 -1 0.057865 108 0.5999999046 18 19 -1 0.0597995 -1 0.173454 -1 -0.0272592 -1 -0.0559942 70 -0.354721576 23 24 -1 0.0299243 261 0.9091652036 25 26 -1 -0.056538 246 750000 27 28 -1 0.0208637 -1 0.313192 29 264 -0.4642856419 1 2 -1 -0.0464083 313 6.5 3 28 7 -0.7238553762 4 21 10 0.4428512454 5 20 0 0.4016023576 6 19 303 0.1082455143 7 8 -1 0.0538045 314 9.5 9 10 -1 -0.0163438 223 500000 11 14 236 -1.156242728 12 13 -1 0.138072 -1 0.00442797 268 -0.8269230127 15 18 242 0.2499999106 16 17 -1 0.000224849 -1 0.788792 -1 0.0603926 -1 -0.0373574 -1 -0.035019 256 0.03619013727 22 23 -1 -0.0419249 316 269 24 27"
   "310 63 25 26 -1 0.03799 -1 -0.00411383 -1 -0.0268928 -1 0.0196064 29 26 0.03301296383 1 20 303 0.002904754831 2 5 184 1.285925269 3 4 -1 0.050734 -1 -0.286709 145 0.5724769831 6 19 240 -1.056057453 7 8 -1 -0.0113937 232 -6.124491215 9 10 -1 -0.292648 224 -0.7291665673 11 16 228 -0.2291666269 12 13 -1 0.0508066 279 -1.052762866 14 15 -1 0.0694507 -1 -0.17276 254 -0.7892150283 17 18 -1 0.0646397 -1 0.021972 -1 -0.0186866 308 200 21 28 84 0.6666665673 22 27 99 0.01065209974 23 26 224 1.333331943 24 25 -1 -0.0113372 -1 -0.0655415 -1 0.0105333 -1 -0.0318122 -1 0.0425567 29 315 0.8766067028 1 2 -1 -0.0797804 246 0.01785713993 3 26 284 -1.00289166 4 13 307 13 5 6 -1 0.0625094 298 -0.3333332837 7 12 14 -0.4999999106 8 9 -1 0.0373673 279 -0.9803815484 10 11 -1 -0.12686 -1 0.0228042 -1 0.0659657 305 0.002074245363 14 17 9 -1.125353694 15 16 -1 -0.189088 -1 0.0779817 218 -0.5874999166 18 23 265 0.09999997914"
   "19 22 307 5 20 21 -1 0.0385824 -1 -0.170621 -1 0.0106406 240 -1.618674874 24 25 -1 0.0252493 -1 -0.055684 247 -0.6666664481 27 28 -1 0.11933 -1 -0.0724312 29 82 0.9999997616 1 28 303 0.0129060261 2 3 -1 -0.0522908 233 -0.6184234023 4 11 11 -0.4999999106 5 10 295 0.02499999478 6 7 -1 -0.0194744 314 9.5 8 9 -1 0.121649 -1 -0.00988582 -1 -0.0506099 7 -0.05348043889 12 27 255 -0.8195423484 13 14 -1 -0.00839664 303 0.1924695075 15 20 65 -1.499999762 16 17 -1 0.118277 204 1.124999881 18 19 -1 -0.00331811 -1 0.0668954 283 -0.6797831059 21 22 -1 0.188848 219 -0.3749999106 23 24 -1 -0.0542136 17 -0.3999999166 25 26 -1 0.122446 -1 0.0396498 -1 -0.0340849 -1 0.050786 29 305 0.007999050431 1 2 -1 -0.0536463 304 0.002031427342 3 4 -1 -0.066176 308 211 5 28 304 0.7093615532 6 27 15 -0.6153845787 7 14 306 0.8651621342 8 13 283 -0.7479110956 9 12 314 8.5 10 11 -1 -0.0339401 -1 0.0283647 -1 0.0273284 -1 0.107529"
   "256 0.09909276664 15 18 122 -0.8152020574 16 17 -1 0.0373754 -1 -0.024128 248 1.115384459 19 20 -1 0.00487395 2 -0.2503542006 21 22 -1 -0.00289778 273 0.624999702 23 26 218 0.2499999404 24 25 -1 0.0589648 -1 0.185435 -1 -0.00241158 -1 -0.0523373 -1 -0.0418578 29 38 0.3333332837 1 24 3 -0.1044178754 2 15 256 -0.8262735605 3 6 225 0.3333332241 4 5 -1 0.0107855 -1 0.0673447 7 -0.7983611822 7 14 269 0.04166665301 8 11 281 -1.55163753 9 10 -1 -0.211617 -1 -0.0535431 313 4.5 12 13 -1 0.0256681 -1 -0.0369743 -1 -0.000679501 130 -0.8571428061 16 17 -1 -0.0333234 248 1.187499404 18 19 -1 0.0457145 280 -0.9811421037 20 23 282 -0.9722184539 21 22 -1 0.0502862 -1 -0.477165 -1 0.00274538 168 0.8689664006 25 28 296 -0.2916665673 26 27 -1 -0.0454061 -1 -0.000320328 -1 -0.040439 29 315 0.8766067028 1 2 -1 -0.0796999 244 0.01562499814 3 28 286 -2.734549522 4 19 307 8 5 8 102 -2.068872452 6 7 -1 -0.0608984"
   "-1 0.0678972 305 0.001714284881 9 10 -1 0.0713524 298 -0.3333332837 11 18 244 -0.7142856121 12 13 -1 0.0410413 268 -0.3437499106 14 17 215 -1.266133428 15 16 -1 -0.0226855 -1 -0.173789 -1 -0.00684043 -1 0.0514147 260 0.7502095699 20 27 267 0.1562499553 21 24 257 0.172492072 22 23 -1 -0.0144247 -1 0.139012 259 0.3124972582 25 26 -1 -0.03417 -1 -0.159786 -1 0.0713363 -1 -0.0625719 29 77 0.7106205225 1 26 93 -0.1072131321 2 7 263 -1.132259488 3 4 -1 -0.0221374 79 -0.8546138406 5 6 -1 0.0886741 -1 0.025721 303 0.039558433 8 9 -1 -0.0545234 37 1.499999285 10 25 13 -0.4999999106 11 24 306 0.08501250297 12 23 213 -1.086533904 13 16 215 -0.5421726108 14 15 -1 -0.0649502 -1 0.0768825 236 -0.3203116059 17 18 -1 0.0869641 214 -0.2723002434 19 20 -1 -0.0144396 5 -0.8736851811 21 22 -1 0.18104 -1 0.00301917 -1 -0.0374255 -1 -0.0265057 -1 0.085745 62 1.499999285 27 28 -1 0.0315991 -1 0.127226 29 264 -0.4999998212"
   "1 2 -1 -0.0461562 304 0.8316112757 3 28 312 2.5 4 25 7 -0.7238553762 5 16 283 -0.738853097 6 11 269 0 7 10 288 750000 8 9 -1 0.0216201 -1 0.349702 -1 -0.0334675 269 500000 12 15 306 0.4424661994 13 14 -1 0.0175835 -1 0.0966191 -1 0.127576 268 0.2124999762 17 20 224 2.187498808 18 19 -1 -0.0359625 -1 0.0699427 304 0.04521339014 21 24 53 0.9025866389 22 23 -1 -0.0296794 -1 0.0491687 -1 0.0221601 295 0.4722221792 26 27 -1 0.0231299 -1 -0.0175299 -1 -0.0797751 29 23 0.2296649367 1 20 303 0.00977383554 2 11 186 0.1468769461 3 4 -1 0.04563 311 1.5 5 6 -1 0.0339213 122 -0.6434257627 7 10 84 -0.3999999166 8 9 -1 0.0437912 -1 -0.154562 -1 -0.00615784 266 -0.2499999702 12 15 306 0.9770019054 13 14 -1 0.0178923 -1 -0.26592 249 -0.04999998957 16 19 172 -1.166666508 17 18 -1 -0.0682524 -1 -0.0167546 -1 0.00254745 84 0.2727272511 21 28 236 -1.312492371 22 23 -1 -0.0432783 276 0.2408023477 24 27 192 -0.4413177371"
   "25 26 -1 -0.0325282 -1 0.0236053 -1 -0.0145813 -1 -0.0285822 29 315 0.8766067028 1 2 -1 -0.0796157 247 0 3 28 172 0.1666666418 4 27 11 -1.333333135 5 8 223 2.999996901 6 7 -1 0.0674527 -1 -0.136492 218 -1.020833135 9 12 282 -2.19998455 10 11 -1 -0.147467 -1 0.0440136 8 -1.355816722 13 14 -1 0.0635227 253 0.4451917708 15 26 169 -0.01244500745 16 17 -1 -0.0346571 22 -0.4999998808 18 21 248 -0.166666612 19 20 -1 0.0274778 -1 -0.103524 169 0 22 25 257 -1.483130574 23 24 -1 -0.0801975 -1 0.0891238 -1 -0.0531767 -1 0.0833232 -1 -0.0541863 -1 -0.0650128 29 36 0.3999999464 1 24 16 0 2 23 13 1000000 3 22 316 543 4 21 279 -1.089211583 5 6 -1 -0.0739976 17 0.1052631363 7 20 277 -1.010145068 8 9 -1 0.198384 269 -0.3749998212 10 11 -1 -0.0468401 122 0.4199160337 12 19 303 0.1082455143 13 16 276 0.4751200378 14 15 -1 -0.0389061 -1 0.0314546 41 0.3076921999 17 18 -1 0.0187938 -1 0.0928167 -1 0.0657768 -1"
   "-0.0372523 -1 -0.0496862 -1 0.149226 -1 -0.0432042 82 0.4999999404 25 26 -1 0.0064229 59 0.4999999106 27 28 -1 0.01121 -1 0.0789461 29 268 -0.2053571343 1 8 306 0.02685395442 2 3 -1 -0.0593851 7 -0.7181621194 4 5 -1 0.0104525 270 1.549999714 6 7 -1 -0.0536769 -1 0.114367 308 101 9 26 310 63 10 13 310 55 11 12 -1 0.0104723 -1 0.0514372 310 87 14 15 -1 -0.0476816 7 -0.403411299 16 21 306 0.01960255951 17 18 -1 -0.029732 273 4.999995232 19 20 -1 0.0251488 -1 0.150181 271 1.999997973 22 23 -1 -0.0151724 49 -0.1143328547 24 25 -1 0.295366 -1 0.0345177 0 -0.1013351828 27 28 -1 0.00698299 -1 -0.0385967 29 146 0.2611518502 1 26 303 0.009172229096 2 9 186 0.1758808494 3 4 -1 0.0384331 222 0.4545454085 5 8 311 1.5 6 7 -1 0.0418116 -1 -0.0140828 -1 -0.0734799 307 55 10 19 240 -1.618674874 11 14 263 -35.99740982 12 13 -1 0.0727764 -1 -0.0207955 220 -0.194444418 15 18 245 -2.749999285 16 17 -1 -0.177899"
   "-1 0.0438222 -1 0.00807113 260 -0.9510096312 20 21 -1 0.0526907 251 -0.6666664481 22 23 -1 -0.0816392 294 0.9687497616 24 25 -1 -0.00811584 -1 -0.0500087 256 -0.8835548162 27 28 -1 0.0151726 -1 -0.0206814 29 315 0.8766067028 1 2 -1 -0.0795285 246 0.01785713993 3 26 284 -1.00289166 4 17 314 9.5 5 8 310 63 6 7 -1 -0.0544167 -1 0.0596546 283 -0.9398591518 9 12 248 -0.5357142091 10 11 -1 -0.18189 -1 -0.0138197 237 -0.8207873702 13 14 -1 0.108733 307 15 15 16 -1 0.0395533 -1 -0.0658393 305 0.002074245363 18 21 9 -1.125353694 19 20 -1 -0.166803 -1 0.0736605 218 -0.5874999166 22 23 -1 -0.101166 308 4 24 25 -1 0.048204 -1 -0.0301536 252 -0.8124998808 27 28 -1 0.138513 -1 -0.0686851 29 33 2.899915934 1 26 257 0.7288937569 2 17 304 0.01431206148 3 4 -1 -0.0651198 59 1.285714149 5 16 42 -1.499999642 6 7 -1 0.053781 305 0.002824444789 8 13 279 -0.6170538068 9 10 -1 -0.0728712 251 -0.4687498808 11 12"
   "-1 0.12958 -1 0.0186423 72 -0.2621124983 14 15 -1 0.0169398 -1 -0.0297206 -1 0.047215 250 0.7499997616 18 25 308 8 19 20 -1 0.0220097 234 0.5170674324 21 22 -1 0.130803 303 0.0003537615412 23 24 -1 0.127866 -1 -0.0487925 -1 -0.0379103 286 -2.916622877 27 28 -1 -0.0375439 -1 0.0650107 29 264 -0.3281249702 1 4 278 -6.129182816 2 3 -1 0.162497 -1 -0.0360995 304 0.8316112757 5 28 312 2.5 6 25 308 248 7 24 192 -0.4149622023 8 11 280 -0.6395431757 9 10 -1 0.0639139 -1 0.00636652 187 0.1207145527 12 23 7 -0.1946321577 13 20 283 -0.2846114933 14 19 314 9.5 15 18 224 -0.7499998212 16 17 -1 0.102418 -1 -0.0419785 -1 0.0142629 -1 0.02581 294 -0.9999989867 21 22 -1 0.300986 -1 -0.0393844 -1 0.0163502 -1 -0.0756062 295 0.4722221792 26 27 -1 0.0223459 -1 -0.0159117 -1 -0.078595 29 187 0.08069829643 1 24 308 8 2 5 307 7 3 4 -1 -0.0781262 -1 -0.0127689 2 -0.05699811503 6 19 284 -1.00289166 7 8 -1 -0.0286495"
   "278 -0.009887663648 9 18 233 -0.3664450347 10 17 314 11.5 11 14 316 247 12 13 -1 0.00422191 -1 0.0476755 275 0.9583330154 15 16 -1 -0.000785023 -1 -0.116115 -1 0.0532549 -1 -0.0167308 71 0.1729046702 20 23 235 1.148633599 21 22 -1 0.0549454 -1 -0.0817495 -1 -0.00640336 260 -0.8347268701 25 26 -1 0.0313014 279 -0.1771733761 27 28 -1 0.0164584 -1 -0.0209032 29 315 0.8726333976 1 2 -1 -0.0797414 244 0.01562499814 3 28 159 -0.7499998808 4 5 -1 -0.0555217 254 0.4885778725 6 27 310 127 7 12 254 -0.3202258945 8 9 -1 -0.0746574 310 60 10 11 -1 -0.0975019 -1 0.0406571 269 0.8124995828 13 20 209 -0.9697518349 14 17 303 0.9842739701 15 16 -1 -0.0398806 -1 0.046085 302 0.5 18 19 -1 0.074906 -1 -0.0236687 240 -7.777715683 21 22 -1 0.0373879 259 0.1607134789 23 24 -1 -0.0372704 208 -0.6224480867 25 26 -1 -0.332954 -1 -0.0017669 -1 0.0849769 -1 -0.0597449 29 34 0.4999999106 1 20 42 -0.9999998212 2 5 222"
   "-0.9886362553 3 4 -1 -0.0457049 -1 0.0491068 17 0 6 19 231 0.8929556608 7 18 303 0.01043619774 8 9 -1 -0.0660477 59 1.399999738 10 17 316 543 11 16 56 -0.4229129851 12 15 104 1.499999642 13 14 -1 0.0166768 -1 0.147411 -1 -0.00981931 -1 -0.0484644 -1 0.0549355 -1 0.119149 -1 -0.0467561 13 -0.1999999583 21 28 313 5.5 22 23 -1 0.0224732 253 -0.07487563789 24 25 -1 0.0482216 200 -0.111111097 26 27 -1 -0.016989 -1 0.153488 -1 -0.00451149 29 218 -0.5874999166 1 2 -1 -0.0399246 304 0.7093615532 3 28 308 101 4 23 28 0.6154760122 5 22 261 -0.8890727162 6 7 -1 -0.0472662 266 1.099999785 8 19 103 -1.399999738 9 10 -1 0.0535994 248 -0.9999989867 11 12 -1 0.188019 186 0.09052088112 13 18 281 -0.7007328868 14 17 197 -0.5714284778 15 16 -1 0.0735473 -1 0.0100658 -1 -0.00528724 -1 0.0262976 307 323 20 21 -1 -0.0429201 -1 0.234856 -1 -0.0203953 1 -0.09384018928 24 27 294 0.9166663885 25 26 -1 -0.0110765 -1"
   "0.0461995 -1 -0.0396122 -1 -0.0512987 29 146 0.2611518502 1 28 303 0.009172229096 2 17 186 0.1179823354 3 6 23 0.2227519155 4 5 -1 0.0467166 -1 0.00243097 311 1.5 7 10 291 0.6249999404 8 9 -1 0.0397113 -1 -0.0950218 130 -0.6666665673 11 14 61 -0.2142857015 12 13 -1 -0.00335311 -1 -0.172443 308 68 15 16 -1 -0.0354557 -1 0.0318648 192 -0.4552225769 18 21 281 -0.4026041329 19 20 -1 -0.0469163 -1 0.00260025 278 -0.009887663648 22 27 304 0.007395505439 23 24 -1 -0.0236985 240 0.02016265318 25 26 -1 0.0134209 -1 0.0499623 -1 -0.0102226 -1 -0.0136904 29 315 0.8726333976 1 2 -1 -0.079678 294 0 3 28 123 0.01034515072 4 27 100 -0.1708313525 5 6 -1 -0.046467 4 -0.9813854098 7 26 8 -1.014009237 8 15 272 750000 9 14 259 1.312490106 10 13 228 -1.562499642 11 12 -1 -0.0563644 -1 0.0447585 -1 -0.13957 -1 -0.100477 291 -0.2045454383 16 25 298 -0.4999998808 17 24 17 -0.4999998808 18 19 -1 0.0379787 303 0.9692437649"
   "20 21 -1 -0.0609765 225 -0.4999997616 22 23 -1 0.0109539 -1 -0.294539 -1 -0.0106017 -1 0.0946453 -1 0.0590458 -1 -0.0307394 -1 -0.0739493 29 33 3.272716522 1 26 316 543 2 25 201 -0.2222221941 3 4 -1 -0.0367273 56 -0.7999808192 5 12 166 -0.2177259177 6 9 13 -0.2857142389 7 8 -1 0.144802 -1 0.0235621 153 0.9230768681 10 11 -1 0.00848819 -1 0.0853711 125 -1.760698915 13 14 -1 0.0544814 7 -0.07186266035 15 22 42 -1.199999809 16 19 214 -0.2848198712 17 18 -1 0.0198116 -1 0.15185 131 -0.1428571194 20 21 -1 -0.0527129 -1 0.0061569 284 0.7999040484 23 24 -1 -0.0615215 -1 0.103675 -1 -0.04261 286 -2.916622877 27 28 -1 -0.036206 -1 0.0622408 29 220 -0.2249999493 1 8 272 -0.2499999404 2 3 -1 -0.0528434 2 -4.08324337 4 7 274 -0.1874999702 5 6 -1 -0.0513678 -1 0.176441 -1 -0.0126076 315 0.9802469015 9 28 279 -1.052762866 10 15 304 0.2833355069 11 12 -1 -0.0384311 316 138 13 14 -1 0.317052 -1 -0.00774085"
   "289 -0.4583332539 16 17 -1 0.0451874 303 0.08808559924 18 27 0 -0.56877774 19 20 -1 0.0370553 254 0.1220607013 21 22 -1 -0.0262994 21 -0.9999998212 23 24 -1 0.172873 308 36 25 26 -1 0.0348047 -1 0.000816287 -1 -0.0165215 -1 0.0567621 29 76 0.5235430598 1 26 99 0.09283752739 2 25 130 -0.8571428061 3 8 107 -0.8181817532 4 5 -1 0.0155931 94 0.1956354976 6 7 -1 -0.106657 -1 -0.0134237 303 0.003647685051 9 14 226 500000 10 11 -1 0.0364783 0 0.07965901494 12 13 -1 -0.307879 -1 0.042338 268 -0.6136363149 15 16 -1 0.0164804 7 -0.6919954419 17 20 269 0.1249999404 18 19 -1 -0.0479611 -1 -0.00754978 145 0.4027389288 21 24 290 -0.3999999166 22 23 -1 -0.0342303 -1 0.0162964 -1 -0.0347533 -1 0.0256484 94 -0.1112986133 27 28 -1 0.0369638 -1 -0.0227856 29 315 0.8726333976 1 2 -1 -0.0796157 247 0 3 28 292 -0.2499999553 4 11 253 -0.5447404981 5 10 190 -2.775557562e-16 6 7 -1 -0.050969 259 0.3999980092 8 9"
   "-1 0.0646794 -1 -0.124819 -1 0.0753946 13 -0.1999999732 12 25 247 -1.387778781e-17 13 24 247 -0.5999999046 14 15 -1 0.0553664 238 -1.118920684 16 19 228 -1.149999738 17 18 -1 -0.11901 -1 0.0446899 305 0.001714284881 20 21 -1 0.0638721 270 0.7187498212 22 23 -1 -0.102933 -1 0.0574586 -1 0.0644433 272 0.09999997914 26 27 -1 0.00352089 -1 -0.0763803 -1 -0.0612881 29 37 0.7142856121 1 20 72 -0.2621124983 2 5 247 0.2083332986 3 4 -1 -0.0234969 -1 0.0395823 19 -0.7142856121 6 15 251 -0.3333332837 7 12 3 -1.139226317 8 9 -1 -0.0210898 254 -0.4476441145 10 11 -1 0.1079 -1 -0.0123207 295 0.2499999553 13 14 -1 -0.0212806 -1 0.0499141 42 -1.249999642 16 17 -1 0.03379 257 0.4695927203 18 19 -1 -0.0444547 -1 -0.00304249 40 0.3333332241 21 24 52 0.6666633487 22 23 -1 -0.0125275 -1 0.0701307 200 -0.111111097 25 26 -1 -0.0290609 246 -0.3749998212 27 28 -1 -0.123358 -1 0.0966593 29 305 0.007999050431 1 2"
   "-1 -0.0464109 220 -1.270833135 3 4 -1 -0.08577 308 248 5 28 304 0.002031427342 6 7 -1 -0.0607792 2 -6.684075356 8 9 -1 0.107966 256 -1.263305306 10 11 -1 -0.0560819 224 -0.7499998212 12 13 -1 0.0867534 310 63 14 21 310 55 15 20 310 31 16 19 310 24 17 18 -1 -0.0103844 -1 0.0409359 -1 -0.0235366 -1 0.0384443 310 77 22 23 -1 -0.0532461 2 -0.9038825035 24 27 314 3.5 25 26 -1 -0.0270418 -1 0.0308952 -1 -0.00400321 -1 -0.0491567 29 26 0.02158706263 1 20 305 0.9643476605 2 17 93 -0.09172898531 3 6 263 -1.009512663 4 5 -1 0.023254 -1 -0.0221069 240 -0.42068398 7 10 255 -0.8195423484 8 9 -1 0.0181228 -1 -0.0167995 315 0.9877551198 11 16 224 -0.6666664481 12 13 -1 -0.0345347 232 -6.124491215 14 15 -1 -0.210865 -1 0.0435953 -1 -0.152965 235 1.344946027 18 19 -1 0.0560341 -1 -0.109012 278 -1.818633199 21 22 -1 0.031201 146 0.1500405818 23 28 118 0.03066296317 24 27 132 0.9999996424 25 26 -1 -0.00369155"
   "-1 -0.0422468 -1 0.0184693 -1 -0.0214797 29 315 0.8726333976 1 2 -1 -0.0795492 169 0 3 28 169 -0.008797541261 4 9 253 0.3875407875 5 6 -1 -0.0264662 221 0.05357142165 7 8 -1 0.1153 -1 0.00138496 222 -0.1249999925 10 27 22 -0.4999998808 11 24 259 0.4423059821 12 21 276 -1.617049813 13 14 -1 0.07121 209 -0.2534460723 15 20 282 -4.3332901 16 17 -1 0.0596332 236 -2.291653395 18 19 -1 -0.107634 -1 -0.00114553 -1 0.0761651 9 -1.647232771 22 23 -1 0.0503195 -1 -0.220256 18 -0.6666662097 25 26 -1 -0.16406 -1 0.0610877 -1 0.0931459 -1 -0.0422017 29 82 1.166666508 1 28 167 0.399997592 2 19 306 0.4056941271 3 18 83 1.199999809 4 13 303 0.01638398133 5 6 -1 -0.0530619 40 0.4999997616 7 12 239 0.5077194571 8 11 69 -0.7124432921 9 10 -1 0.0792154 -1 -0.0124247 -1 -0.0580744 -1 0.0288087 9 -0.387776345 14 15 -1 0.101698 276 0.3895774186 16 17 -1 -0.0506668 -1 0.0567025 -1 -0.0727247 307 3 20 23 147 -0.4912950993"
   "21 22 -1 0.28474 -1 0.0797746 34 0.111111097 24 25 -1 -0.0110936 270 2.624997377 26 27 -1 0.0308153 -1 0.254055 -1 0.0437645 29 220 -0.2249999493 1 6 291 -2.593749285 2 3 -1 0.0918462 226 -0.3749999404 4 5 -1 -0.0520319 -1 -0.00988947 266 1.041666269 7 28 310 63 8 15 310 55 9 12 259 -0.1874988228 10 11 -1 0.0366659 -1 -0.00929143 307 3 13 14 -1 0.144052 -1 0.0367189 310 77 16 17 -1 -0.0475729 306 0.04073333368 18 25 268 0.5714284778 19 22 270 0.09374997765 20 21 -1 -0.0638788 -1 -0.00997331 192 -0.5817428231 23 24 -1 0.265672 -1 0.0221307 227 2.324999571 26 27 -1 0.0103634 -1 0.100862 -1 -0.0303709 29 268 -0.9999998808 1 2 -1 0.0329755 138 0.4169807434 3 28 303 0.00977383554 4 11 256 0.071978122 5 6 -1 0.0392424 316 84 7 10 226 500000 8 9 -1 -0.0259249 -1 -0.348641 -1 0.0145756 192 -0.4149622023 12 15 281 -0.3425451815 13 14 -1 -0.0477571 -1 0.000318496 269 0.1249999553 16 25 307 10 17 22"
   "16 -0.3809522986 18 19 -1 -0.100975 315 0.4302721024 20 21 -1 0.0377728 -1 -0.0510013 307 48 23 24 -1 0.0193436 -1 -0.0222463 279 -0.9327960014 26 27 -1 0.039561 -1 0.00432728 -1 -0.0164651 29 315 0.8726333976 1 2 -1 -0.0794777 294 0 3 28 244 -0.9249998331 4 5 -1 0.062263 305 0.001714284881 6 9 18 -0.6666662097 7 8 -1 -0.19194 -1 0.065285 254 -0.3202258945 10 19 310 127 11 12 -1 -0.0931465 306 0.04073333368 13 18 22 -0.4999998808 14 17 240 -2.984335184 15 16 -1 -0.0115531 -1 -0.183433 -1 0.0616379 -1 -0.0455714 246 -0.1249999776 20 23 234 -1.116365314 21 22 -1 0.0304265 -1 0.116745 8 -0.874370873 24 25 -1 0.0313657 1 -0.8377702832 26 27 -1 -0.138997 -1 -0.00815801 -1 -0.0723465 29 29 0.2499991655 1 12 70 -0.4503361881 2 5 14 -0.2499999702 3 4 -1 0.0882861 -1 0.000899216 316 417 6 9 178 -0.3999998271 7 8 -1 -0.0559491 -1 0.00068057 246 1125000 10 11 -1 -0.0537115 -1 0.121811 258 0.9053804278"
   "13 28 270 1.249998808 14 17 293 0.4374998808 15 16 -1 -0.012606 -1 0.0268521 229 0.2968749702 18 21 13 -0.7142856121 19 20 -1 0.131805 -1 -0.0681284 228 0.874999702 22 23 -1 0.0764307 308 32 24 25 -1 -0.0358149 266 0 26 27 -1 0.0282916 -1 0.603627 -1 0.0805495 29 261 -0.8890727162 1 2 -1 -0.0512507 304 0.7093615532 3 28 308 205 4 27 172 -0.9999997616 5 8 94 0.08061297983 6 7 -1 0.0400432 -1 0.00237674 122 -0.9962091446 9 14 94 0.1652799547 10 13 99 -0.8673681617 11 12 -1 0.00494142 -1 0.129694 -1 -0.0106057 248 -0.9999989867 15 16 -1 0.13332 296 6.124993801 17 24 7 0.2095866799 18 23 283 0.3718773127 19 20 -1 -0.00386142 1 0.3355014622 21 22 -1 0.0165448 -1 0.0879243 -1 -0.0289807 274 0.4166665375 25 26 -1 0.114213 -1 -0.0338617 -1 -0.0346026 -1 -0.0504367 29 122 -0.9962091446 1 4 94 0.2969889343 2 3 -1 -0.0550866 -1 0.0288238 7 0.1328398883 5 20 214 0.4328461885 6 17 0 -0.1720799357 7 14"
   "305 0.5669066906 8 11 286 -2873.563232 9 10 -1 0.066673 -1 -0.00256467 271 0.6249998212 12 13 -1 -0.00654432 -1 -0.0541822 280 -0.9085770249 15 16 -1 -0.0486369 -1 0.0199584 230 0.3867946565 18 19 -1 -0.021192 -1 -0.0922245 253 0.1611629725 21 22 -1 0.0541437 13 -0.5263157487 23 26 288 0.624999702 24 25 -1 -0.0230243 -1 -0.241098 226 500000 27 28 -1 0.0224604 -1 -0.163403 29 315 0.8726333976 1 2 -1 -0.0794043 244 0.01562499814 3 28 46 1.223461628 4 27 305 0.001497944235 5 8 21 -1.999999285 6 7 -1 -0.128172 -1 0.0774164 244 -0.9249998331 9 12 294 -0.1666666418 10 11 -1 0.0873593 -1 -0.0163721 252 -0.4374999404 13 20 314 10.5 14 19 259 0.3999980092 15 18 307 7 16 17 -1 0.0577389 -1 -0.031657 -1 -0.11683 -1 -0.085521 306 0.05012866482 21 22 -1 0.0338591 316 4 23 24 -1 0.116151 229 0.7499996424 25 26 -1 -0.0577549 -1 0.128578 -1 0.0613005 -1 -0.0560821 29 33 3.272716522 1 28 258 0.8880009651"
   "2 25 304 0.01431206148 3 4 -1 -0.0645967 14 -0.1249999851 5 20 221 0.5749998689 6 19 122 0.4338091016 7 16 305 0.003952585626 8 15 251 -0.4687498808 9 12 225 0 10 11 -1 0.0576308 -1 0.254805 215 -0.2949343622 13 14 -1 -0.0516631 -1 0.0994706 -1 -0.0154196 219 -0.5249999166 17 18 -1 -0.0696212 -1 0.0713026 -1 0.0692262 255 1.381267309 21 22 -1 -0.0170992 212 0.1978434771 23 24 -1 -0.0237389 -1 0.430062 246 750000 26 27 -1 0.0353198 -1 0.265149 -1 0.042113 29 305 0.004354107659 1 2 -1 -0.0579811 304 0.8316112757 3 28 220 -1.124999762 4 5 -1 -0.0652003 2 -6.684075356 6 9 315 0.6229050159 7 8 -1 -0.0237094 -1 0.174677 283 -1.28949964 10 11 -1 -0.0376454 308 101 12 23 28 0.6154760122 13 22 245 -0.9431817532 14 15 -1 0.0510671 256 -1.065912247 16 17 -1 -0.0562495 218 -0.6124999523 18 19 -1 -0.0492649 224 -0.7499998212 20 21 -1 0.10534 -1 0.012087 -1 -0.0185174 1 0.1377362609 24 27 225 0.4687498808"
   "25 26 -1 -0.0219403 -1 0.0217087 -1 -0.0444098 -1 -0.0745099 29 268 -0.9999998808 1 2 -1 0.0309356 15 -0.4999999702 3 12 306 0.8651621342 4 11 240 0.1731472313 5 10 282 -0.2142849416 6 7 -1 -0.0257388 278 -0.6668845415 8 9 -1 0.021363 -1 -0.0109338 -1 0.0260297 -1 -0.0676028 130 -0.6666665673 13 22 132 -0.6666665673 14 15 -1 0.0304039 61 -0.6666664481 16 17 -1 0.0123158 93 0.02337423339 18 21 71 0.1310430765 19 20 -1 -0.00133099 -1 -0.149792 -1 -0.0325549 23 0.3153951466 23 28 7 0.1328398883 24 27 260 0.657749176 25 26 -1 0.0182216 -1 -0.0402409 -1 0.0415477 -1 -0.00615016 29 315 0.8726333976 1 2 -1 -0.0793268 294 0 3 28 310 60 4 5 -1 -0.0620779 253 0.3087608218 6 27 13 -0.6999999285 7 14 212 -1.203808308 8 13 236 -3.571408272 9 10 -1 0.0691073 218 -1.458332896 11 12 -1 0.053649 -1 -0.058479 -1 0.0544847 230 -1.192563176 15 20 291 -0.4027777314 16 19 8 -1.590334654 17 18 -1 0.0500128 -1 -0.0386271"
   "-1 0.0774908 307 50 21 26 19 -1.357142687 22 25 242 -0.3999999166 23 24 -1 -0.307433 -1 -0.019347 -1 -0.0618379 -1 0.0226759 -1 0.0659045 -1 -0.070643 29 59 0.4999999106 1 24 77 -0.5797015429 2 3 -1 0.0364528 303 0.09437434375 4 9 284 0.7572010756 5 6 -1 -0.0392082 287 0.07499998808 7 8 -1 -0.0315229 -1 0.0902812 277 -0.5864046216 10 13 270 250000 11 12 -1 -0.050451 -1 0.126818 222 -0.9531248808 14 19 7 -2.535672665 15 16 -1 0.0284087 53 0.09736910462 17 18 -1 -0.0805374 -1 0.089017 316 232 20 21 -1 0.0427151 306 0.2256476581 22 23 -1 0.0118734 -1 -0.0775463 82 0.3333332837 25 26 -1 -0.00335445 106 0.9999989867 27 28 -1 0.0296291 -1 0.105756 29 261 -0.8890727162 1 2 -1 -0.04845 304 0.8316112757 3 28 103 -0.9999997616 4 5 -1 0.020375 218 -0.1071428433 6 11 235 -1.747897863 7 10 254 -1.489357829 8 9 -1 0.459299 -1 0.0531905 -1 -0.0185724 304 0.008792823181 12 13 -1 -0.0195364 220 1.249999881"
   "14 27 26 0.2595952451 15 26 223 2.124999046 16 23 296 6.124993801 17 22 254 -0.6002033949 18 19 -1 -0.0305016 224 2.562498808 20 21 -1 0.0140556 -1 0.125249 -1 0.123976 245 -0.8333332539 24 25 -1 0.184857 -1 0.0469391 -1 -0.000172327 -1 -0.0421587 -1 -0.0744797 29 146 0.1402929723 1 18 316 252 2 11 189 -0.4427821636 3 6 269 -0.1562499553 4 5 -1 0.0632998 -1 -0.0299376 185 -0.000564357324 7 10 92 0.4904199839 8 9 -1 0.0253007 -1 -0.0315612 -1 -0.0066589 313 4.5 12 17 279 -0.004280854948 13 14 -1 0.0425873 4 -0.7345924973 15 16 -1 -0.0655249 -1 0.0179688 -1 0.00140522 315 0.1666666716 19 22 87 -0.1176470444 20 21 -1 0.0416655 -1 -0.051396 271 0.7916663885 23 26 307 2 24 25 -1 -0.0477901 -1 0.00644952 309 31 27 28 -1 0.0324899 -1 -0.0347515 29 315 0.8726333976 1 2 -1 -0.0792435 172 0.08333332837 3 28 92 -0.004512113053 4 5 -1 -0.0260261 4 -0.9813854098 6 27 8 -1.590334654 7 8 -1 0.0531787 241"
   "-1.66666615 9 10 -1 0.0597994 287 -1.124999762 11 16 234 -1.085031033 12 15 22 -0.5999999046 13 14 -1 -0.158345 -1 0.0275524 -1 -0.350266 289 -0.2499999702 17 24 260 -0.1686487943 18 23 282 -2.428559303 19 20 -1 0.0409465 305 0.001714284881 21 22 -1 0.063831 -1 -0.106431 -1 0.0506301 14 -0.3749999404 25 26 -1 -0.14115 -1 -0.0122171 -1 0.0471818 -1 -0.0473879 29 310 1 1 4 94 -0.1780556291 2 3 -1 0.174655 -1 -0.0114616 16 0 5 26 19 -0.6999999285 6 9 58 0.4999998808 7 8 -1 0.0119192 -1 0.062734 237 0.002467067679 10 15 13 3.999998569 11 12 -1 -0.025332 246 0.9999996424 13 14 -1 -0.0186561 -1 0.238167 288 0.1499999762 16 17 -1 -0.0198233 270 1.424999714 18 25 212 0.84483248 19 24 303 0.02213495411 20 21 -1 -0.0651343 273 0.8124998212 22 23 -1 0.0473843 -1 -0.0497165 -1 0.118265 -1 0.16616 78 -0.2964134216 27 28 -1 0.0172844 -1 -0.0443339 29 305 0.004354107659 1 2 -1 -0.0557061 285 0.3958349824"
   "3 18 308 211 4 17 31 0.9013977647 5 14 248 -0.9999989867 6 7 -1 0.142459 266 -1.208333135 8 9 -1 -0.0877464 146 0.1402929723 10 13 7 0.2203412354 11 12 -1 0.00902342 -1 -0.0227912 -1 0.0229638 303 0.006647820119 15 16 -1 0.0592287 -1 -0.0344267 -1 -0.0415403 267 0.4999994934 19 22 305 0.5669066906 20 21 -1 -0.0553056 -1 -0.0140257 309 64 23 26 315 0.8646864891 24 25 -1 -0.0495416 -1 0.324311 0 0.1887755245 27 28 -1 0.0481917 -1 -0.00556947 29 76 0.4483281076 1 24 99 0.09283752739 2 23 130 -0.7142856121 3 10 61 -0.6666664481 4 5 -1 0.00759483 72 0.2607533932 6 9 48 0.05813088641 7 8 -1 -0.00554299 -1 -0.12961 -1 -0.0139144 117 -0.04046258703 11 12 -1 0.0262922 189 -0.2576988339 13 16 235 -0.1714475006 14 15 -1 -0.0403655 -1 0.00862592 310 121 17 22 187 0.06084121764 18 21 246 250000 19 20 -1 0.0428378 -1 -0.062597 -1 -0.00354251 -1 -0.00487789 -1 0.0240913 94 -0.03456617892 25 26 -1 0.0247321"
   "283 -0.8170725703 27 28 -1 0.0206903 -1 -0.0274408 29 316 77 1 28 247 0 2 27 310 60 3 4 -1 -0.0666121 253 0.3134190142 5 24 13 -0.2222221941 6 21 126 0.2499999404 7 20 226 -0.3448275328 8 19 286 -10.1536808 9 12 255 -1.007599711 10 11 -1 -0.0182887 -1 0.0749566 305 0.002310694894 13 16 220 -2.343749523 14 15 -1 -0.182883 -1 0.0467159 8 -1.12613368 17 18 -1 0.00763957 -1 -0.0875522 -1 0.0520467 -1 -0.0552686 272 0.1249999553 22 23 -1 0.0105452 -1 -0.0748241 113 -0.6249999404 25 26 -1 -0.0683778 -1 0.0755762 -1 -0.0575423 -1 -0.0791598 29 36 0.2499999404 1 24 277 -0.6936935782 2 3 -1 -0.0530067 283 -0.6797831059 4 9 250 0.3437499106 5 8 275 0.4062499106 6 7 -1 0.0655003 -1 0.334766 -1 -0.0451445 42 -1.499999642 10 17 30 0.6493225694 11 14 236 -0.5277754068 12 13 -1 0.122074 -1 -0.0118148 232 -0.4519841969 15 16 -1 -0.0570948 -1 0.221508 17 0.1052631363 18 23 257 0.9031105638 19 20 -1 -0.00820322"
   "239 0.3135123253 21 22 -1 0.0923913 -1 -0.0148604 -1 -0.0448846 11 -0.4999999404 25 26 -1 0.0427529 130 -0.9999998808 27 28 -1 0.149702 -1 -0.00484546 29 256 -0.7891161442 1 18 250 125000 2 17 15 -0.692307651 3 16 248 -0.2499999106 4 11 266 -0.2499999553 5 6 -1 -0.055407 226 0.4285713732 7 10 261 -0.3339007497 8 9 -1 0.187079 -1 0.00819143 -1 -0.0455246 315 0.08852458745 12 15 28 0.07426286489 13 14 -1 0.0166 -1 0.251547 -1 -0.0390515 -1 -0.0684449 -1 0.100983 248 0.9374995232 19 28 240 -1.398767591 20 23 212 0.9918586016 21 22 -1 0.0251696 -1 -0.072754 274 0.2708332837 24 27 278 -0.8923133016 25 26 -1 -0.0312884 -1 0.00817414 -1 -0.0183424 -1 0.0184404 29 256 -0.9346600771 1 4 216 -0.2323664874 2 3 -1 0.000511199 -1 0.0393364 312 2.5 5 20 293 0.9999998212 6 17 315 0.97430408 7 12 304 0.04521339014 8 9 -1 0.0240626 223 -0.3749996126 10 11 -1 -0.0643437 -1 0.00366574 231 -0.1481503844 13 14"
   "-1 -0.0253866 266 0.3749999106 15 16 -1 0.0405365 -1 -0.404005 167 -0.3333316743 18 19 -1 -0.0678636 -1 -0.00498806 69 0.3888474107 21 28 311 1.5 22 25 2 -0.7394940853 23 24 -1 -0.0108936 -1 0.0303735 5 0.9542509317 26 27 -1 -0.0220909 -1 0.0577339 -1 -0.0356739 29 316 77 1 28 294 0 2 27 244 -0.9249998331 3 4 -1 0.0548278 305 0.001497944235 5 8 225 -0.624999702 6 7 -1 -0.125424 -1 0.0748513 222 -0.4583332837 9 18 50 0 10 13 241 -1.349999785 11 12 -1 0.0778137 -1 -0.0159444 307 4 14 15 -1 0.0193067 308 14 16 17 -1 -0.111366 -1 -0.00987397 246 -0.1249999776 19 22 234 -1.085031033 20 21 -1 0.0117937 -1 0.0967913 137 -0.1599999815 23 24 -1 -0.0760904 147 0 25 26 -1 0.0255649 -1 -0.0736224 -1 -0.0691845 -1 -0.079067 29 133 -0.1666666418 1 2 -1 -0.0248644 78 -0.6782774329 3 10 167 0.7499962449 4 5 -1 0.0277476 222 -0.06249999627 6 7 -1 0.0630211 264 -0.02499999478 8 9 -1 -0.0392104 -1 0.342923"
   "77 0.3913032413 11 22 267 0.3749999106 12 17 229 1.41666615 13 16 51 -0.6066715121 14 15 -1 0.0380505 -1 -0.0278761 -1 0.156704 8 -0.2373877019 18 19 -1 0.0417044 255 1.064913511 20 21 -1 -0.0168107 -1 0.224484 62 1.857142568 23 26 80 0.6666664481 24 25 -1 0.0022078 -1 0.0473804 77 0.6693787575 27 28 -1 -0.0173926 -1 0.117744 29 261 -0.8890727162 1 2 -1 -0.0460585 304 0.8316112757 3 28 275 1.949999571 4 27 190 -0.285712868 5 8 259 1.18749404 6 7 -1 0.0138455 -1 -0.0714166 278 -0.02318211272 9 20 281 -0.5888389945 10 17 222 -2.374999523 11 12 -1 0.174991 5 -0.4416878819 13 14 -1 -0.021506 257 -0.3341963291 15 16 -1 -0.0604046 -1 0.0445828 267 0.1428571194 18 19 -1 -0.045892 -1 -0.0119624 2 0.2303120047 21 26 266 0.7916663885 22 25 13 -1.333333135 23 24 -1 -0.0676909 -1 0.0179606 -1 -0.0361474 -1 -0.023386 -1 0.059657 -1 -0.073147 29 266 -1.395833135 1 2 -1 0.047375 243 -0.9999997616 3 6 288"
   "125000 4 5 -1 -0.0275103 -1 -0.274456 278 -0.009887663648 7 16 284 -1.322272658 8 9 -1 -0.0348182 258 -0.03661061078 10 11 -1 0.0243633 305 0.9705046415 12 15 16 -0.3999999166 13 14 -1 -0.0414794 -1 0.00350839 -1 0.0479535 288 -2.081668171e-17 17 18 -1 0.0129792 315 0.9678111672 19 26 279 -0.09949540347 20 21 -1 0.0338559 130 -0.6363635659 22 23 -1 -0.0478699 315 0.1623931676 24 25 -1 -0.0474711 -1 -0.00479661 230 -0.3049036264 27 28 -1 0.00712503 -1 -0.300942 29 316 77 1 28 294 0 2 27 125 2.643723249 3 26 102 -3.272716522 4 5 -1 -0.0583153 244 -0.9249998331 6 7 -1 0.0677957 254 0.4885778725 8 25 267 0.1874999106 9 20 257 -0.1646843404 10 19 307 21 11 12 -1 0.0339659 307 54 13 18 305 0.001714284881 14 15 -1 0.052717 271 0.7499997616 16 17 -1 -0.0791328 -1 0.117349 -1 0.0237962 -1 0.0967582 286 -3.919728279 21 22 -1 0.0118947 277 -0.7435498834 23 24 -1 -0.106792 -1 0.00765705 -1 0.0565771"
   "-1 -0.0523624 -1 -0.0676826 -1 -0.0789666 29 29 0.2499991655 1 20 70 -0.4503361881 2 5 14 -0.2499999702 3 4 -1 0.0747053 -1 -0.000433299 316 417 6 19 5 0.02360744216 7 18 50 -0.5229472518 8 15 236 -0.2291651368 9 14 287 -0.04999998957 10 11 -1 -0.0462129 179 0.3333332241 12 13 -1 0.396232 -1 0.0320859 -1 0.0385907 212 -1.23418808 16 17 -1 -0.081911 -1 0.00693441 -1 -0.031205 -1 -0.0442513 258 0.9053804278 21 28 270 1.249998808 22 27 305 0.002074245363 23 24 -1 0.0734537 264 1.115384459 25 26 -1 0.00474928 -1 -0.0737975 -1 0.0697855 -1 0.0653186 29 218 -0.4999998808 1 6 307 82 2 3 -1 -0.0451948 228 0.3593749404 4 5 -1 -0.022777 -1 0.0832052 266 0.8749991059 7 28 308 200 8 27 209 -5.879361629 9 12 310 247 10 11 -1 0.237417 -1 0.00452029 256 -1.263305306 13 14 -1 -0.0490528 95 0.2600254714 15 24 130 -0.3999999166 16 21 138 -0.2088373005 17 18 -1 0.00526603 193 -0.1582641304 19 20 -1 0.101484"
   "-1 0.030506 172 -0.9999997616 22 23 -1 0.0285862 -1 0.000842162 24 0.4392137825 25 26 -1 -0.059728 -1 -0.00174928 -1 -0.0307953 -1 -0.0252519 29 278 -1.818633199 1 4 208 -2.998543024 2 3 -1 -0.0733374 -1 0.0380244 33 2.516083717 5 26 122 -0.9962091446 6 9 311 2.5 7 8 -1 -0.0456902 -1 0.0259848 7 0.2203412354 10 23 282 -0.2142849416 11 12 -1 -0.0106245 76 0.4977746606 13 22 305 0.9472084045 14 21 307 55 15 20 10 -0.09868358821 16 19 260 0.7723885179 17 18 -1 0.0435725 -1 -0.0704659 -1 0.00537038 -1 -0.00745063 -1 0.0475268 -1 -0.0126755 243 125000 24 25 -1 0.0283276 -1 -0.170205 263 -0.5986730456 27 28 -1 0.00153454 -1 -0.0491653 29 315 0.8726333976 1 2 -1 -0.0788617 172 0.1666666418 3 28 182 -0.9999998808 4 7 19 1.999999642 5 6 -1 -0.0758417 -1 0.1 46 0.4731263518 8 25 165 -0.001144059002 9 14 65 0.7499998212 10 13 257 -0.1786312312 11 12 -1 -0.0525618 -1 0.0121249 -1 0.038164 259 0.2083321214"
   "15 24 310 123 16 21 260 -0.1686487943 17 18 -1 -0.0708631 46 0.02600642852 19 20 -1 0.0709948 -1 -0.0669443 228 -1.562499642 22 23 -1 -0.0519591 -1 0.0560127 -1 -0.0422797 310 60 26 27 -1 -0.0597615 -1 0.0550051 -1 -0.0498673 29 34 0.4999999106 1 20 307 2 2 9 284 -0.3254733682 3 4 -1 -0.0523826 295 0.04166666046 5 6 -1 -0.00748341 298 0.1874999702 7 8 -1 0.0288552 -1 0.188705 89 1.249999642 10 17 105 1.142856956 11 16 228 -0.5312498808 12 15 18 -0.2499999702 13 14 -1 -0.0315442 -1 0.0435163 -1 -0.0227652 -1 0.027928 104 0.9999997616 18 19 -1 0.00615826 -1 0.0987481 305 0.003275237745 21 22 -1 0.0960495 272 1.583332777 23 26 167 0.4444424808 24 25 -1 0.00426622 -1 0.0539575 244 0.2692307532 27 28 -1 0.0213169 -1 -0.131737 29 304 0.8316112757 1 28 261 -0.8890727162 2 5 273 -0.5999999046 3 4 -1 0.0466202 -1 -0.06345 315 0.9802469015 6 23 303 0.7549455166 7 18 275 1.949999571 8 17 278 -1.710716963"
   "9 10 -1 -0.0374735 245 -0.9431817532 11 14 310 179 12 13 -1 0.00296579 -1 0.0731783 256 -0.7891161442 15 16 -1 -0.0346495 -1 0.00307795 -1 0.0585668 222 -2.374999523 19 22 243 -0.8374999166 20 21 -1 0.267584 -1 -0.056134 -1 -0.0460155 266 0.166666612 24 25 -1 -0.00383499 250 -0.4999999106 26 27 -1 -0.0122258 -1 0.111846 -1 -0.0723111 29 272 0.5937499404 1 24 220 -0.178571403 2 3 -1 0.00987281 291 -0.7874999046 4 7 295 -0.0624999702 5 6 -1 0.00235451 -1 0.0702151 307 63 8 17 294 -0.4999997616 9 10 -1 -0.081103 256 0.6352572441 11 16 99 0.8357346654 12 15 235 0.7940781713 13 14 -1 -0.0187563 -1 0.0330372 -1 -0.0588832 -1 0.0218593 265 0.3749999106 18 21 308 14 19 20 -1 -0.0109132 -1 0.0338559 249 -0.08333332092 22 23 -1 -0.04489 -1 -0.000503182 248 0.7812498212 25 28 285 -0.6378837824 26 27 -1 -0.0159754 -1 0.0391173 -1 -0.0170087 29 316 77 1 28 244 0.01562499814 2 27 159 -0.7499998808 3 4"
   "-1 -0.0515924 4 -0.9813854098 5 22 8 -1.014009237 6 15 233 -1.333969593 7 14 0 -1.611456275 8 13 18 -0.4999998808 9 12 8 -1.462216139 10 11 -1 0.0435649 -1 -0.135115 -1 0.0570347 -1 -0.098368 -1 0.0337155 291 -0.2045454383 16 21 293 -0.4999999106 17 18 -1 0.0373557 22 -0.4999998808 19 20 -1 -0.146913 -1 -0.0256681 -1 0.0702459 136 0.5999999046 23 26 280 -0.8154544234 24 25 -1 0.066356 -1 0.00517912 -1 -0.0601448 -1 -0.0517078 -1 -0.0787476 29 257 0.8852184415 1 26 304 0.01431206148 2 3 -1 -0.0558265 33 3.272716522 4 23 42 -1.499999642 5 6 -1 0.0407098 305 0.002824444789 7 12 225 -0.2083332688 8 9 -1 -0.0395594 315 0.5585215688 10 11 -1 -0.00491109 -1 0.100142 201 0.1999999583 13 14 -1 -0.0253482 306 0.01355251949 15 16 -1 -0.050053 102 -0.2248970717 17 22 209 -0.9001259804 18 21 168 1.775989294 19 20 -1 -0.0644249 -1 0.106417 -1 0.0637655 -1 -0.000112353 223 -0.1964285374 24 25 -1 -0.0344957"
   "-1 0.0522751 250 0.8124995828 27 28 -1 0.0510642 -1 -0.053002 29 305 0.004354107659 1 2 -1 -0.0513425 78 0.4636314809 3 28 69 0.4227112532 4 25 95 0.1356705427 5 24 122 -0.8152020574 6 7 -1 0.0499076 7 0.2095866799 8 23 268 0.5714284778 9 20 306 0.0278744176 10 11 -1 -0.0203934 10 0.4428512454 12 19 315 0.97430408 13 16 296 6.124993801 14 15 -1 0.00908059 -1 0.112156 292 -0.0624999702 17 18 -1 -0.0187735 -1 0.114989 -1 -0.0257354 294 -0.1249999553 21 22 -1 0.124475 -1 0.0238597 -1 -0.032193 -1 -0.0192047 2 0.6120250821 26 27 -1 0.020941 -1 0.143862 -1 -0.0130069 29 146 0.2611518502 1 20 77 -0.1637846678 2 7 311 1.5 3 4 -1 0.00811622 99 0.001493317774 5 6 -1 -0.0343597 -1 -0.00150183 24 -0.0009196011815 8 9 -1 0.0292495 197 -1.199999809 10 13 138 -0.7030214071 11 12 -1 0.0400195 -1 -0.0538123 240 0.05324762315 14 15 -1 0.00158729 291 0.04807692021 16 17 -1 0.0495567 2 0.2896029055 18 19 -1"
   "-0.0227869 -1 0.0572907 226 0.874999702 21 24 266 -0.6607142091 22 23 -1 0.0246685 -1 -0.0172599 303 0.001009741216 25 28 217 -4444.444336 26 27 -1 -0.316834 -1 0.00405758 -1 0.0388869 29 315 0.8726333976 1 2 -1 -0.078629 305 0.001497944235 3 6 21 -1.999999285 4 5 -1 -0.0962362 -1 0.0737556 69 1.226319671 7 28 308 1 8 11 291 -0.1249999851 9 10 -1 0.0119021 -1 0.12702 303 0.05863045156 12 13 -1 -0.0681387 13 2.333332539 14 27 284 -1.00289166 15 24 234 -1.980306387 16 17 -1 -0.0341044 258 -1.095287442 18 19 -1 -0.0171118 308 20 20 23 227 -0.2812499404 21 22 -1 0.0761292 -1 -0.00727676 -1 0.107814 2 -0.1080150977 25 26 -1 -0.044262 -1 0.0505668 -1 0.145102 -1 0.05237 29 78 -0.6782774329 1 14 108 -1000000 2 5 225 0.6874996424 3 4 -1 0.0184899 -1 0.316508 133 -0.4999999106 6 7 -1 -0.05225 310 6 8 9 -1 0.172783 53 -0.04184124246 10 11 -1 -0.0490402 150 -0.7999998331 12 13 -1 -0.0208348 -1 0.0642834"
   "58 0.5333331823 15 22 267 0.3499999344 16 17 -1 -0.0192223 76 0.6477873325 18 21 236 -0.2916642427 19 20 -1 0.0300006 -1 -0.0180236 -1 0.0682644 19 -0.7142856121 23 24 -1 0.048986 128 1.999999762 25 28 284 0.8151865005 26 27 -1 -0.00895381 -1 0.0849563 -1 0.11901 29 266 -1.395833135 1 2 -1 -0.0797299 304 0.8316112757 3 28 103 -1.124999881 4 7 101 -0.02761681937 5 6 -1 0.0320889 -1 -0.00769652 218 -0.1071428433 8 19 248 -0.3749999106 9 14 315 0.6348167658 10 11 -1 -0.0202401 228 -0.3333332837 12 13 -1 -0.00745573 -1 0.151391 295 0.2499999702 15 18 221 0.1249999851 16 17 -1 -0.0323508 -1 0.00251919 -1 -0.054114 303 0.0005927180173 20 23 189 1.26199615 21 22 -1 -0.0374287 -1 0.343504 10 0.7441816926 24 27 25 0.2794995904 25 26 -1 0.0158493 -1 -0.00106092 -1 -0.0431633 -1 -0.0708833 29 308 248 1 28 266 -1.395833135 2 3 -1 0.0429216 243 -0.9999997616 4 5 -1 -0.0295947 185 0.08294771612 6 21 258"
   "-0.04953175783 7 14 209 -5.879361629 8 9 -1 -0.134848 227 0.02499999478 10 13 249 0.833333075 11 12 -1 0.0128894 -1 -0.0589969 -1 0.0377225 188 -0.4859924018 15 18 211 0.3050028384 16 17 -1 -0.0648328 -1 -0.00446425 223 750000 19 20 -1 0.00802902 -1 -0.0561411 304 0.2704432011 22 27 86 -0.1249999851 23 24 -1 0.0041707 5 -1.363606691 25 26 -1 0.0718026 -1 -0.0311026 -1 0.00569585 -1 0.0403895 29 316 77 1 28 247 0 2 27 310 59 3 4 -1 -0.06729 253 0.4232804477 5 26 197 -0.1777777672 6 7 -1 -0.0367938 243 -0.2499999106 8 23 27 -0.04712353647 9 10 -1 0.109608 226 -0.4791665971 11 20 305 0.002310694894 12 15 230 -2.018363714 13 14 -1 -0.0470712 -1 0.0562231 281 -0.8667159677 16 19 286 -6.740693569 17 18 -1 0.0246134 -1 -0.0555036 -1 -0.13948 146 0 21 22 -1 0.0548084 -1 -0.0489771 218 0.1499999762 24 25 -1 0.0106591 -1 -0.0740514 -1 0.0613841 -1 -0.0533985 -1 -0.0785038 29 303 0.1924695075 1 14 284"
   "-0.0285309609 2 3 -1 -0.0425264 278 0.06283567101 4 5 -1 -0.0224458 310 3 6 7 -1 0.116652 291 0.1607142687 8 9 -1 0.0534314 282 -0.145832479 10 11 -1 0.0337391 290 1.124998927 12 13 -1 -0.0301923 -1 0.11837 257 0.4199291766 15 26 310 123 16 21 246 0.1749999672 17 18 -1 -0.0741299 242 0.3749998212 19 20 -1 0.0364556 -1 -0.076255 268 -0.6428570747 22 25 273 1.187499404 23 24 -1 -0.0246288 -1 0.0817643 -1 0.044753 53 0.5615215302 27 28 -1 0.0346524 -1 0.129997 29 272 -0.9999998808 1 2 -1 -0.059933 2 -4.08324337 3 6 223 0.9999995232 4 5 -1 0.0143293 -1 0.106621 95 -0.2839443386 7 8 -1 -0.0325848 220 1.093749881 9 28 310 63 10 21 310 55 11 20 310 31 12 19 310 29 13 16 69 0.3328031898 14 15 -1 -0.017841 -1 0.0411739 26 0.1457817554 17 18 -1 0.158033 -1 0.0135207 -1 -0.0237918 -1 0.0320271 310 77 22 23 -1 -0.0470698 250 2.874997139 24 25 -1 -0.00148974 304 0.03015177324 26 27 -1 -0.048908 -1 0.0876727"
   "-1 -0.0328395 29 0 0.7167333961 1 28 310 63 2 13 310 55 3 10 145 0.7793595195 4 9 248 1.249998808 5 8 241 -0.7916663885 6 7 -1 0.12064 -1 0.0132833 -1 -0.0503725 -1 -0.0512864 307 7 11 12 -1 -0.119178 -1 -0.025504 2 -0.04769827798 14 25 310 114 15 22 1 -0.712524116 16 17 -1 0.0429918 310 95 18 21 310 77 19 20 -1 0.026986 -1 -0.0321874 -1 0.0366499 296 6.124993801 23 24 -1 -0.00518964 -1 -0.0703051 215 0.8041626811 26 27 -1 0.0270029 -1 -0.0933947 -1 0.0205307 29 315 0.8726333976 1 2 -1 -0.0783668 115 0 3 26 115 -0.003510124749 4 15 195 -0.1249999851 5 14 5 -1.196472049 6 7 -1 -0.0397551 285 -0.8362583518 8 9 -1 0.0635052 226 0.08928570151 10 11 -1 -0.0798285 56 -0.0692961365 12 13 -1 0.127742 -1 -0.0295949 -1 -0.036063 222 -0.1249999925 16 25 259 0.3749977648 17 22 314 9.5 18 19 -1 0.041909 33 4.590238571 20 21 -1 0.00775144 -1 -0.0967158 303 0.9909460545 23 24 -1 0.0106334 -1 -0.130122 -1"
   "0.0919522 259 -0.517854929 27 28 -1 -0.0633384 -1 -0.00126716 29 232 -2.140637875 1 2 -1 -0.0539546 303 0.1924695075 3 18 284 -0.0285309609 4 5 -1 -0.0393555 278 0.06283567101 6 9 216 -0.08891957253 7 8 -1 0.0294455 -1 -0.0358563 307 203 10 17 310 3 11 12 -1 0.104388 286 -0.9019570351 13 16 264 0.2499999106 14 15 -1 -0.00815426 -1 0.170125 -1 0.00291402 -1 0.126766 38 0.1632652879 19 22 251 -0.2708332837 20 21 -1 0.0364783 -1 -0.00847546 238 0.1380521506 23 28 246 0.1874999553 24 27 314 14.5 25 26 -1 -0.0296312 -1 0.111532 -1 0.0810918 -1 0.125152 29 259 1.18749404 1 28 190 -0.285712868 2 5 99 0.07387825847 3 4 -1 0.0183522 -1 -0.00625234 275 1.949999571 6 27 291 0.6916666031 7 26 130 -0.3999999166 8 15 132 -0.1999999583 9 10 -1 -0.0200569 61 -0.444444418 11 12 -1 -0.0116298 93 0.0501377061 13 14 -1 0.0912987 -1 0.0187058 15 -0.4999999702 16 25 315 0.101333335 17 20 253 -1.257250071 18 19"
   "-1 -0.0611684 -1 0.072896 172 -0.9999998212 21 22 -1 0.0251598 228 -0.4249999225 23 24 -1 -0.0659224 -1 -0.00712493 -1 -0.0225836 -1 0.0314686 -1 0.062175 -1 -0.0487001 29 308 107 1 24 223 1.999997973 2 15 285 0.3397948146 3 6 220 -1.458332896 4 5 -1 0.0567977 -1 -0.00556363 267 0.4791665971 7 8 -1 0.0278972 316 53 9 12 314 4.5 10 11 -1 -0.236527 -1 -0.0206873 270 3.999995947 13 14 -1 0.00722448 -1 -0.102254 297 0.3749998212 16 19 274 0.849999845 17 18 -1 0.000154279 -1 -0.0877646 227 0.1249999851 20 21 -1 -0.11095 219 0.3823528588 22 23 -1 0.0429844 -1 -0.0526262 247 0.2916665673 25 28 316 767 26 27 -1 0.0433784 -1 -0.0629889 -1 -0.00105843 29 316 77 1 28 294 0 2 27 244 -0.9249998331 3 4 -1 0.048203 252 -0.4999998808 5 18 234 -0.9461113811 6 17 238 -0.99915272 7 14 14 -0.4999999106 8 9 -1 0.0208456 214 -0.695499897 10 13 19 -2.249999523 11 12 -1 0.0536333 -1 -0.127932 -1 0.000555159 17 -0.4999999106"
   "15 16 -1 0.0645899 -1 -0.12951 -1 0.0577509 306 0.1245080456 19 24 220 -2.343749523 20 21 -1 -0.130853 246 0 22 23 -1 0.0385057 -1 -0.0585516 315 0.995824635 25 26 -1 -0.0496746 -1 0.0773517 -1 -0.0642994 -1 -0.0782203 29 82 1.166666508 1 28 232 -2.140637875 2 3 -1 -0.0633165 303 0.1924695075 4 17 285 -0.3200261593 5 6 -1 -0.0658377 293 0.2499999553 7 8 -1 -0.0373879 310 3 9 12 286 -0.5447819829 10 11 -1 -0.0569623 -1 0.142024 42 -1.499999642 13 16 6 0.5555524826 14 15 -1 0.0961619 -1 -0.0510484 -1 -0.00368749 34 0.4999997616 18 25 257 0.4065188766 19 20 -1 -0.00782215 314 1.5 21 22 -1 0.128224 242 0.4374998808 23 24 -1 -0.019204 -1 0.0881689 42 2.999999046 26 27 -1 0.0576938 -1 -0.060598 -1 0.0317087 29 304 0.8316112757 1 28 44 -0.9999998212 2 3 -1 -0.018678 218 -0.5874999166 4 9 307 83 5 6 -1 -0.0461896 248 -0.3749999106 7 8 -1 0.208233 -1 0.00137776 266 0.8749991059 10 27 213 1.390618086"
   "11 26 95 0.2600254714 12 25 197 -0.5714284778 13 16 1 -0.1167679876 14 15 -1 0.0450943 -1 0.00115297 192 0.297962904 17 18 -1 0.000714724 59 1.999999523 19 24 216 -0.4223948419 20 23 229 0.09374997765 21 22 -1 0.0346163 -1 0.1463 -1 0.0171113 -1 -0.074631 -1 -0.0138075 -1 -0.0724193 -1 -0.0217239 -1 -0.0697242 29 279 -1.052762866 1 10 294 0.1249998733 2 9 300 0.5 3 6 259 0.3124972582 4 5 -1 -0.00150736 -1 0.0502218 213 0.3281233609 7 8 -1 0.0571454 -1 -0.204693 -1 0.0510912 272 0.5833331347 11 24 220 -0.178571403 12 15 283 -1.040291667 13 14 -1 -0.0821744 -1 0.0101345 293 -0.7499996424 16 17 -1 -0.100392 247 0.7249999046 18 23 243 -0.2499999702 19 20 -1 -0.0287139 16 -0.1428571194 21 22 -1 -0.0195866 -1 0.00717179 -1 -0.0243172 278 -0.9792333841 25 26 -1 -0.11716 262 1.046266556 27 28 -1 0.0245257 -1 -0.0651908 29 316 77 1 28 276 -1.191608548 2 17 233 -1.333969593 3 12 274 0.562499702 4 11"
   "233 -1.521271825 5 10 10 -1.087476373 6 7 -1 0.0570266 214 -0.06591841578 8 9 -1 -0.0723849 -1 0.0405187 -1 -0.0681224 -1 -0.166428 267 0.2499999702 13 16 303 0.08808559924 14 15 -1 -0.0434297 -1 0.0753042 -1 -0.0112376 230 -0.8522069454 18 25 295 -1.499999642 19 22 5 -0.7146784067 20 21 -1 0.0583254 -1 -0.117572 15 -0.1999999881 23 24 -1 -0.0638374 -1 0.0383363 315 0.9407407641 26 27 -1 -0.0548972 -1 0.035765 -1 -0.0780643 29 197 0.9999998808 1 26 89 1.249999642 2 19 11 -0.1249999851 3 12 101 -0.3819774687 4 9 84 0.5714284778 5 6 -1 0.0253624 226 -0.04545454308 7 8 -1 0.00391 -1 0.248294 39 -0.1249999851 10 11 -1 -0.0521174 -1 0.00262076 44 -2.142856836 13 14 -1 0.0741561 214 -0.7473622561 15 16 -1 -0.0667135 141 0.4294970632 17 18 -1 -0.0297128 -1 0.0310498 79 3.843621492 20 25 67 1.111111045 21 24 305 0.07590124011 22 23 -1 0.0392495 -1 -0.0422518 -1 0.076152 -1 0.135374 59 2.222221851"
   "27 28 -1 0.0150341 -1 0.096931 29 132 0.9999996424 1 26 2 -0.8830468655 2 13 281 0.7040300369 3 12 314 2.5 4 5 -1 -0.0284771 245 -0.6341462731 6 9 247 -0.8124995828 7 8 -1 -0.044296 -1 0.038337 231 -1.020771146 10 11 -1 -0.0843106 -1 0.00837572 -1 -0.0450551 268 -0.2053571343 14 15 -1 -0.0360272 15 -1.759999752 16 17 -1 -0.154606 213 1.187493086 18 25 283 -0.8248975873 19 20 -1 -0.0421089 266 1.099999785 21 22 -1 0.00342716 307 323 23 24 -1 -0.0505218 -1 0.22486 -1 -0.0779594 149 -0.3333332837 27 28 -1 0.0454284 -1 0.0034799 29 41 1.124999881 1 28 7 -0.403411299 2 11 286 -2873.563232 3 4 -1 0.0448828 35 -0.5555555224 5 8 56 0.4755230248 6 7 -1 0.0310528 -1 -0.0120573 2 -0.4074945152 9 10 -1 -0.00617625 -1 -0.036303 130 -0.5833333135 12 19 132 0.9999996424 13 14 -1 -0.00154231 2 0.09265869111 15 18 268 0.01785713993 16 17 -1 -0.00857429 -1 -0.13515 -1 0.0188754 316 106 20 21 -1 -0.00280995"
   "187 0.1274154633 22 25 226 -1.124999762 23 24 -1 -0.0991517 -1 0.0376381 236 -0.1527769268 26 27 -1 -0.0284125 -1 0.0240542 -1 -0.0176983 29 316 77 1 28 172 0.08333332837 2 23 182 -0.9999998808 3 6 19 1.999999642 4 5 -1 -0.0711576 -1 0.0892245 46 0.4731263518 7 18 165 -0.001144059002 8 13 65 0.7499998212 9 12 161 -1.08683455 10 11 -1 0.0452441 -1 -0.043571 -1 0.0373395 55 0.002887195675 14 15 -1 0.0312328 38 0.3076922894 16 17 -1 -0.0539398 -1 0.0720661 310 60 19 20 -1 -0.0533931 278 -0.9252723455 21 22 -1 0.0137731 -1 0.0730678 271 0.4642856419 24 25 -1 -0.0715896 306 0.05383221805 26 27 -1 0.0674233 -1 -0.0663408 -1 -0.0778971 29 133 -0.1666666418 1 2 -1 -0.0217461 100 -1.019443035 3 4 -1 0.0738942 190 0.3999986649 5 12 276 0.5781559348 6 11 21 -0.9999997616 7 8 -1 0.0306122 246 -0.08333330601 9 10 -1 -0.0615098 -1 -0.0112553 -1 0.0280743 25 0.06073949113 13 16 307 4 14 15 -1 0.0823652"
   "-1 -0.0020727 101 -0.4589363039 17 20 120 -0.000657794124 18 19 -1 -0.0051084 -1 0.191313 260 0.4542554617 21 28 45 0.5714284778 22 25 282 -0.1071420908 23 24 -1 -0.0461749 -1 0.0476027 3 -2.421704054 26 27 -1 -0.0918368 -1 0.118742 -1 -0.0376161 29 256 -0.7891161442 1 14 207 -0.7602830529 2 13 261 -0.5958818197 3 4 -1 -0.0349424 235 -1.747897863 5 8 212 -0.94104743 6 7 -1 0.276093 -1 -0.0238424 307 5 9 10 -1 0.11454 299 0.5 11 12 -1 0.0254822 -1 -0.0572199 -1 -0.0524782 226 1250000 15 26 226 1.187499404 16 23 213 1.187493086 17 22 78 0.4636314809 18 19 -1 0.00774927 255 -1.084831715 20 21 -1 0.183479 -1 -0.0130387 -1 -0.0535677 307 323 24 25 -1 -0.0550591 -1 0.140454 286 -3.633267164 27 28 -1 0.151084 -1 0.0170414 29 220 -2.343749523 1 2 -1 0.0854903 146 0.1402929723 3 16 157 -0.3749999404 4 5 -1 -0.00812285 315 0.9919678569 6 15 303 0.01043619774 7 10 69 0.3240338862 8 9 -1 0.0319654 -1"
   "-0.00763015 77 -0.1637846678 11 14 290 0 12 13 -1 -0.0381447 -1 0.00100293 -1 0.0116647 -1 -0.0387412 315 0.1666666716 17 20 18 0.2499999404 18 19 -1 -0.047425 -1 0.0421673 271 0.7916663885 21 22 -1 0.00248389 36 0.3999999464 23 24 -1 -0.00871291 267 0.2499999404 25 28 1 -0.5351006389 26 27 -1 0.0303102 -1 -0.174354 -1 -0.0333065 29 315 0.8726333976 1 2 -1 -0.0777121 247 0 3 28 2 -1.814297318 4 9 18 -0.4999998808 5 8 237 -1.434524536 6 7 -1 -0.0230403 -1 -0.218646 -1 -0.00149923 305 0.001497944235 10 11 -1 0.0748589 270 -0.0624999702 12 17 310 123 13 14 -1 -0.0764783 17 -0.3478260338 15 16 -1 0.0385938 -1 -0.0434078 226 -1.54166615 18 19 -1 0.114446 226 -1.249999762 20 21 -1 -0.109686 292 -0.2499999553 22 27 234 -1.643054962 23 26 244 -0.4999999106 24 25 -1 0.0662617 -1 -0.0834956 -1 0.0815057 -1 0.00290437 -1 -0.0505921 29 201 -0.2222221941 1 2 -1 -0.0253994 219 -0.3749999106 3 10 248 -0.02499999478"
   "4 9 255 -0.7038409114 5 8 294 -0.312499851 6 7 -1 -0.0631745 -1 0.135277 -1 -0.0650952 -1 -0.0486749 14 -0.1249999851 11 28 221 0.5833332539 12 27 130 0.2142857015 13 22 310 123 14 15 -1 -0.0254963 146 -1.763162136 16 17 -1 0.275326 132 0.2499999404 18 21 8 -0.05274891481 19 20 -1 0.0351233 -1 -0.041694 -1 -0.0562768 57 -1.199999809 23 24 -1 0.154366 107 0.09999998659 25 26 -1 0.0576612 -1 -0.0315134 -1 0.0757919 -1 -0.00296377 29 304 0.8316112757 1 28 266 -1.395833135 2 3 -1 -0.0745675 248 -0.7692307234 4 5 -1 0.0698407 78 0.4636314809 6 27 69 0.4227112532 7 26 95 0.1356705427 8 25 249 -0.06249998882 9 12 226 -0.1874999553 10 11 -1 -0.00150847 -1 0.0366583 269 -0.1249999776 13 14 -1 -0.0503263 315 0.1666666716 15 16 -1 0.0369675 271 1.111111045 17 18 -1 -0.0125845 224 0.4999994934 19 20 -1 -0.0249294 168 0.1718205065 21 24 273 0 22 23 -1 0.200374 -1 0.0592028 -1 -0.00504914 -1 -0.0173804"
   "-1 0.0212697 -1 -0.0123339 -1 -0.0682001 29 62 -5.999997139 1 8 271 0.7499998212 2 3 -1 0.00102344 290 0.06249999255 4 5 -1 0.0359374 15 -0.7999998927 6 7 -1 0.00286476 -1 -0.112455 0 -0.1566521376 9 20 305 0.5063171983 10 15 315 0.1666666716 11 14 276 -0.08542697877 12 13 -1 -8.32995e-05 -1 -0.046999 -1 0.00515072 224 0.2749999464 16 19 263 -0.4422815442 17 18 -1 -0.0254284 -1 0.0386307 -1 -0.0304313 122 -0.900801897 21 22 -1 -0.0189826 311 2.5 23 26 255 0.8990556002 24 25 -1 0.0249472 -1 -0.0307967 99 -0.6521784663 27 28 -1 -0.1564 -1 -0.00265379 29 315 0.8726333976 1 2 -1 -0.0775194 195 0.1666666418 3 28 193 -0.002007459989 4 19 85 0.4999997616 5 16 244 -0.0624999851 6 13 280 -2.484664202 7 8 -1 -0.0783467 104 1.833333015 9 12 276 -1.191608548 10 11 -1 0.0165809 -1 -0.0296095 -1 0.107159 211 0.4395534992 14 15 -1 -0.0888029 -1 0.0468175 298 -0.1999999583 17 18 -1 0.0576375 -1 -0.0683226"
   "51 0.3685485423 20 27 55 0.002887195675 21 22 -1 0.0253697 38 0.1249999851 23 26 232 -1.502645016 24 25 -1 -0.162289 -1 -0.0395951 -1 0.0296955 -1 0.0608182 -1 -0.0579714 29 231 -1.095824122 1 2 -1 -0.0357933 161 -1.929406762 3 4 -1 0.189409 7 -0.1120237485 5 22 234 0.7414865494 6 21 258 0.101729922 7 10 50 -0.4044101834 8 9 -1 0.0417978 -1 -0.00762886 286 -0.9596793652 11 16 249 0.2708332837 12 15 225 0.166666612 13 14 -1 0.113288 -1 -0.0159779 -1 0.187565 5 0.1160715967 17 20 6 0.333332032 18 19 -1 0.0554625 -1 -0.0214223 -1 -0.0142959 -1 -0.0313851 231 0.8929556608 23 28 125 -2.199961662 24 25 -1 0.0808414 271 -0.4374997914 26 27 -1 0.084639 -1 -0.0490854 -1 0.0792973 29 10 0.7441816926 1 28 103 -1.124999881 2 3 -1 0.0167951 218 -0.1071428433 4 9 248 -0.3749999106 5 8 315 0.6348167658 6 7 -1 -0.017073 -1 0.0868873 -1 -0.0189749 274 0.2916666269 10 19 296 6.124993801 11 18 288 0.5999999046"
   "12 17 268 0.1874999702 13 16 306 0.01818112843 14 15 -1 -0.058435 -1 -0.00172902 -1 0.0157448 -1 0.0342291 -1 0.0958141 258 -0.2170303911 20 21 -1 -0.0451449 61 -1.749999523 22 23 -1 0.176403 226 1250000 24 25 -1 -0.00541891 298 0.2083332986 26 27 -1 0.145199 -1 0.013539 -1 -0.0350575 29 7 -2.830753565 1 4 273 375000 2 3 -1 -0.033822 -1 0.186468 255 -1.804414868 5 8 240 -0.2918069959 6 7 -1 0.071884 -1 -0.0109738 285 0.3958349824 9 22 276 0.2233695388 10 17 294 8.881784197e-16 11 14 308 4 12 13 -1 -0.029406 -1 -0.000617538 190 0.9999949932 15 16 -1 0.0163133 -1 -0.0239985 307 7 18 19 -1 -0.0796526 134 -0.3749999404 20 21 -1 -0.0382427 -1 -0.00245309 267 0.3749998212 23 26 279 0.1653986871 24 25 -1 0.0471304 -1 0.00529298 22 -0.8333331943 27 28 -1 0.168403 -1 -0.00945382 29 316 77 1 28 305 0.001497944235 2 5 21 -1.999999285 3 4 -1 -0.0723076 -1 0.0707536 294 -0.6666665673 6 9 247 -0.3999999166"
   "7 8 -1 0.0868409 -1 -0.0440796 298 -1.031249762 10 11 -1 0.0792113 22 -0.4878048301 12 19 291 -0.2187499404 13 18 6 -3.999966621 14 15 -1 0.0377011 307 5 16 17 -1 0.00119917 -1 -0.0722482 -1 0.0438828 160 -0.1666666418 20 23 246 -0.1249999776 21 22 -1 0.018381 -1 -0.0651515 238 -0.7282904983 24 25 -1 0.0361729 226 -0.7321427464 26 27 -1 -0.166131 -1 -0.00181979 -1 -0.0773128 29 316 543 1 20 232 -2.140637875 2 3 -1 -0.0536028 79 -1.777735114 4 9 193 1.477047682 5 8 236 0.04347803444 6 7 -1 0.0811056 -1 0.00774085 -1 -0.0751974 59 0.4999999106 10 19 216 -0.1403375715 11 16 269 -0.4687498808 12 13 -1 -0.0841169 281 0.7647511363 14 15 -1 0.00979634 -1 0.0903191 255 1.381267309 17 18 -1 -0.0182737 -1 0.192386 -1 0.0211437 250 0.9374997616 21 26 23 -1.101492763 22 23 -1 0.14175 21 -1.333333135 24 25 -1 0.0717078 -1 -0.0537284 13 -0.4999999106 27 28 -1 0.126961 -1 -0.0262387 29 261 -0.8890727162"
   "1 4 273 -0.5999999046 2 3 -1 0.0423244 -1 -0.0581927 248 -0.7692307234 5 6 -1 0.0674076 273 0 7 20 110 -3.999995947 8 11 7 -0.08234604448 9 10 -1 0.0251029 -1 0.231433 0 -0.7198616862 12 15 79 -0.1498121321 13 14 -1 -0.0269384 -1 0.0368077 253 0.3929640949 16 17 -1 -0.0172856 237 -0.7862539887 18 19 -1 0.163234 -1 0.0317892 273 0.3749998212 21 24 212 0.9918586016 22 23 -1 -0.0100945 -1 -0.0832408 219 -0.3999999166 25 26 -1 0.072135 212 -0.1018652022 27 28 -1 -0.0248085 -1 0.007638 29 76 0.4084295332 1 22 99 0.07387825847 2 21 130 -0.3999999166 3 12 15 0.3846153617 4 11 278 -0.1565293223 5 6 -1 -0.0065299 94 0.1106608137 7 10 254 0.5808978677 8 9 -1 -0.108275 -1 0.0320117 -1 -0.0128586 -1 0.00311427 117 -0.04046258703 13 14 -1 0.0248007 187 -0.1197926402 15 18 223 2.374997616 16 17 -1 -0.0114987 -1 -0.0878499 310 114 19 20 -1 0.0147625 -1 -0.00526412 -1 0.0196082 94 -0.03456617892 23 24 -1"
   "0.0208249 184 0.380856216 25 28 77 0.8449640274 26 27 -1 -0.0111074 -1 0.0582761 -1 -0.040202 29 315 0.8726333976 1 2 -1 -0.0770924 136 -0.9999996424 3 4 -1 -0.056746 2 -1.814297318 5 12 18 -0.4999998808 6 9 237 -1.434524536 7 8 -1 -0.0221952 -1 -0.184965 0 -1.374032378 10 11 -1 0.0274088 -1 -0.0855987 305 0.001497944235 13 14 -1 0.0739515 268 0.1428571343 15 22 310 114 16 19 248 -0.4999998808 17 18 -1 0.0430011 -1 -0.0674069 240 -0.9674690962 20 21 -1 0.0198717 -1 -0.0791661 2 -0.4619202018 23 24 -1 0.0505421 316 1 25 26 -1 0.117295 236 -2.174984694 27 28 -1 0.0383609 -1 -0.0524812 29 80 0.5999999642 1 22 77 -0.4183430374 2 7 305 0.7290325165 3 6 168 -0.6375175118 4 5 -1 0.222549 -1 0.0253423 -1 -0.055367 92 1.261022687 8 21 271 0.3749998212 9 20 259 -0.2291655242 10 13 73 -0.3045896888 11 12 -1 0.0227856 -1 -0.0360995 235 -0.4179323018 14 15 -1 0.0461509 231 0.8929556608 16 19 298 0.8124995828"
   "17 18 -1 -0.0185945 -1 0.0607202 -1 0.108962 -1 -0.0311728 -1 0.0713099 74 0.1568466127 23 24 -1 0.00250384 307 30 25 26 -1 0.0107974 152 0.111111097 27 28 -1 0.110019 -1 0.0151606 29 272 -0.9999998808 1 2 -1 -0.0556778 2 -4.08324337 3 6 246 -0.08333330601 4 5 -1 -0.0452275 -1 0.0548825 256 -1.263305306 7 12 248 -0.3749999106 8 11 271 0.166666612 9 10 -1 -0.00821555 -1 0.290107 -1 -0.06163 28 0.6154760122 13 26 308 107 14 17 218 -0.6124999523 15 16 -1 -0.0394747 -1 0.00739688 291 0.404761821 18 23 1 -0.09384018928 19 22 263 0.1166642159 20 21 -1 0.00985271 -1 -0.0855922 -1 -0.042583 316 355 24 25 -1 -0.0214653 -1 0.101797 227 -0.1874999106 27 28 -1 0.0655401 -1 -0.0263395 29 269 -0.1999999583 1 8 307 10 2 7 18 -0.9999989867 3 4 -1 -0.219268 6 -0.7499934435 5 6 -1 -0.0773293 -1 0.0292848 -1 0.0335331 269 0 9 12 7 -2.351372957 10 11 -1 -0.0589241 -1 -0.00861785 220 -0.178571403 13 14 -1 0.0200801"
   "241 1.093749762 15 28 293 0.8249998093 16 27 290 0 17 18 -1 -0.0149686 247 1.874998093 19 26 265 0.2812499404 20 25 307 65 21 24 150 -1.166666508 22 23 -1 0.0775878 -1 -0.0010892 -1 0.0482498 -1 -0.00287567 -1 -0.114866 -1 -0.0239072 -1 0.0316044 29 315 0.8726333976 1 2 -1 -0.0768541 195 0.1666666418 3 28 46 1.223461628 4 27 188 -0.0001208916729 5 16 296 -0.5999999642 6 11 257 -0.1861597449 7 10 287 -1.718749523 8 9 -1 0.0766204 -1 -0.0373413 -1 0.0643462 268 0.6499999762 12 15 73 0.1246319935 13 14 -1 -0.0669898 -1 -0.0160796 -1 0.0447294 236 -4.166635513 17 18 -1 0.0811333 234 -0.9849162698 19 26 304 0.004591601901 20 25 4 -1.073772788 21 22 -1 0.0207022 227 -0.3749996126 23 24 -1 0.0470992 -1 -0.170073 -1 -0.0529696 -1 0.0476399 -1 0.0411713 -1 -0.0547793 29 288 -0.4999997616 1 2 -1 -0.0490765 303 0.1924695075 3 12 284 -0.0285309609 4 5 -1 -0.0352565 278 0.06283567101 6 7 -1 -0.019333"
   "135 0.9999998808 8 9 -1 0.00777803 55 0.355817914 10 11 -1 0.0174262 -1 0.118575 294 -0.3333332837 13 14 -1 -0.0650276 268 -0.6249999404 15 26 250 -0.4444443583 16 17 -1 0.113999 273 0.9999989867 18 23 246 -0.3749998212 19 22 24 -0.02628924698 20 21 -1 0.296424 -1 0.00199508 -1 -0.0317805 272 -0.1249999851 24 25 -1 0.0843879 -1 -0.0956929 195 0.1999999583 27 28 -1 0.0129896 -1 0.0579294 29 210 0.9540166855 1 28 310 63 2 9 266 1.041666269 3 8 259 -0.2678556144 4 5 -1 0.0328573 217 -3.499926567 6 7 -1 -0.0761954 -1 0.00691152 -1 -0.0457555 310 77 10 11 -1 -0.0460354 102 -2.265290976 12 13 -1 0.0340309 2 -0.8830468655 14 19 314 3.5 15 16 -1 -0.0264098 306 0.9770019054 17 18 -1 0.013762 -1 0.211369 268 -0.2053571343 20 21 -1 -0.0359718 277 -0.7751247287 22 23 -1 -0.0288982 294 8.881784197e-16 24 27 280 -0.04447398335 25 26 -1 0.0395782 -1 -0.0398833 -1 -0.00859834 -1 -0.0741569 29 279 -1.052762866"
   "1 14 300 0.5 2 9 259 0.2678556144 3 6 281 -1.706899166 4 5 -1 -0.0316801 -1 0.0165066 246 2125000 7 8 -1 0.0533434 -1 -0.0983778 316 100 10 11 -1 -0.238951 230 -1.042711258 12 13 -1 -0.107212 -1 0.0724765 33 2.516083717 15 26 272 0.5937499404 16 23 203 -0.9999996424 17 18 -1 -0.0214218 315 0.9877551198 19 22 220 -0.178571403 20 21 -1 0.0141191 -1 -0.00342577 -1 -0.0662802 156 2000000 24 25 -1 0.0194013 -1 -0.167753 65 0.9999998212 27 28 -1 -0.00899891 -1 -0.0607268 29 294 0 1 28 244 -0.9249998331 2 3 -1 0.0433404 278 -0.9792333841 4 11 303 0.9692437649 5 8 26 0.4924161136 6 7 -1 -0.0887317 -1 -0.0046247 240 -5.233241081 9 10 -1 0.0277552 -1 -0.0947467 226 -1.424999714 12 13 -1 0.104423 295 -1.145833135 14 19 272 0.348837167 15 18 1 -0.8377702832 16 17 -1 -0.136034 -1 -0.0199174 -1 0.0411649 276 -1.22612834 20 23 16 0.1428571194 21 22 -1 0.048671 -1 -0.0676103 315 0.9407407641 24 25 -1 -0.0807692"
   "230 -0.8522069454 26 27 -1 -0.0303765 -1 0.0426197 -1 -0.0723506 29 83 1.199999809 1 22 167 0.399997592 2 9 40 0.5999999046 3 4 -1 -0.0166086 179 -0.3333332837 5 6 -1 -0.0549498 282 0.149998948 7 8 -1 0.0479048 -1 -0.0124917 38 0.2727272511 10 17 307 3 11 14 287 0.2499999404 12 13 -1 0.00231208 -1 0.141226 62 -0.1904761493 15 16 -1 -0.0690576 -1 0.00140974 101 -0.7052512765 18 19 -1 0.147195 250 -0.4687498808 20 21 -1 0.284785 -1 0.0208362 30 0.9458668232 23 28 152 0.9999998808 24 25 -1 0.0597826 155 0.6666664481 26 27 -1 -0.042256 -1 0.085775 -1 -0.0507933 29 44 -0.9999998212 1 2 -1 -0.0173902 208 0.8646788597 3 28 240 0.1998143196 4 27 272 -0.6730768681 5 6 -1 -0.0354086 274 0.2499999404 7 24 288 0.5999999046 8 11 296 2.749997139 9 10 -1 0.00652753 -1 0.114453 315 0.183116883 12 23 310 30 13 20 308 48 14 19 263 0 15 18 208 0.1468447596 16 17 -1 0.891639 -1 0.0791867 -1 0.0442738 -1 0.00338057"
   "304 0.3576883376 21 22 -1 -0.00188217 -1 0.208115 -1 0.024663 261 -0.2351047993 25 26 -1 -0.0451879 -1 0.00301713 -1 -0.0138588 -1 -0.0371997 29 146 0.1402929723 1 24 77 -0.1637846678 2 7 308 107 3 6 311 1.5 4 5 -1 0.00223524 -1 -0.0234224 -1 0.0223954 240 0.1731472313 8 17 157 -0.3749999404 9 10 -1 -0.00944139 0 -0.124449268 11 14 251 0.8437498808 12 13 -1 0.00655566 -1 -0.0542173 219 0.2916665673 15 16 -1 0.054743 -1 0.012796 308 1 18 19 -1 -0.0430709 293 0.1249999776 20 23 270 -0.7499998212 21 22 -1 -0.0595708 -1 0.0653096 -1 0.00812773 280 -0.07023201138 25 26 -1 -0.0180073 294 0.8035713434 27 28 -1 0.00946376 -1 -0.016556 29 316 77 1 28 310 60 2 5 270 0.5312498808 3 4 -1 -0.0903736 -1 0.0190831 310 80 6 11 266 0.4843749404 7 10 246 -0.1249999776 8 9 -1 0.0970714 -1 0.0138937 -1 -0.0382787 310 94 12 13 -1 -0.0651213 36 0.3749999404 14 23 27 0.0008733270806 15 20 190 -2.775557562e-16 16"
   "17 -1 -0.0487185 310 123 18 19 -1 -0.0411744 -1 0.0440279 50 0.223201409 21 22 -1 -0.0410284 -1 0.00895316 267 0.2812499404 24 25 -1 0.0526811 87 0.5999999046 26 27 -1 -0.0474167 -1 0.0782388 -1 -0.0763563 29 89 1.249999642 1 28 268 0.5999999642 2 27 130 -0.9999998808 3 8 44 0.2758620381 4 5 -1 -0.00966723 250 0.2916666269 6 7 -1 0.00948497 -1 0.284832 306 0.4056941271 9 26 236 -0.2499985993 10 17 259 -0.1944435835 11 12 -1 -0.00739239 265 0.1249998733 13 14 -1 0.0683327 295 0.2031249702 15 16 -1 -0.0123087 -1 0.0591665 153 0.4999997616 18 23 238 -0.8193081617 19 22 237 -0.5529663563 20 21 -1 -0.0684399 -1 0.0924125 -1 -0.0241584 76 -0.01400433294 24 25 -1 -0.0436916 -1 0.0208069 -1 -0.0545474 -1 -0.0544386 -1 0.0259383 29 304 0.8316112757 1 28 308 248 2 27 266 -1.395833135 3 4 -1 -0.0751355 15 -0.9999998808 5 18 217 -6899.641602 6 7 -1 -0.0762904 228 0.5624998808 8 15 19 1.999997973 9 14"
   "266 -0.03124999255 10 13 288 750000 11 12 -1 0.0341278 -1 0.235445 -1 -0.00255418 -1 -0.119848 226 0.04999998957 16 17 -1 0.105264 -1 0.00477776 213 1.187493086 19 26 256 -0.7746472359 20 21 -1 -0.03188 95 0.2600254714 22 25 266 1.099999785 23 24 -1 0.00749127 -1 -0.0321234 -1 -0.0220884 -1 -0.0656299 -1 -0.0354742 -1 -0.0665092 29 41 1.124999881 1 26 19 -0.5714284778 2 3 -1 -0.00749023 310 63 4 7 250 0.6249998212 5 6 -1 -0.0156811 -1 0.0244512 310 90 8 9 -1 0.0363947 16 -0.4285713732 10 15 304 0.1712454855 11 12 -1 0.0146429 307 11 13 14 -1 -0.147383 -1 -0.0244844 269 -0.1249999776 16 17 -1 0.0462885 273 0.02083333023 18 19 -1 -0.00867496 265 0.3749999106 20 21 -1 0.030167 310 128 22 25 225 0.01785713993 23 24 -1 0.0344118 -1 -0.0374546 -1 0.0193308 76 0.0002589050564 27 28 -1 -0.0361106 -1 0.00192307 29 294 0 1 28 244 0.01562499814 2 27 291 -0.2187499404 3 24 276 -1.617049813 4 7 286 -2.734549522"
   "5 6 -1 0.037268 -1 -0.0374843 307 47 8 17 1 -0.9623310566 9 12 6 -3.999966621 10 11 -1 0.0378432 -1 -0.0738976 284 -1.00289166 13 16 292 -0.2499998808 14 15 -1 0.0646 -1 -0.00973292 -1 -0.04552 293 -0.1249998733 18 21 244 -0.9999996424 19 20 -1 0.10733 -1 -0.0358681 314 6.5 22 23 -1 -0.0288171 -1 0.0964944 250 -0.1666666418 25 26 -1 0.0567509 -1 -0.0354087 -1 -0.0501514 -1 -0.0709124 29 208 1.007637382 1 28 304 0.01431206148 2 3 -1 -0.0608225 234 0.7414865494 4 27 257 0.8692324162 5 24 270 1.424999714 6 19 305 0.002824444789 7 12 225 -0.2083332688 8 9 -1 -0.0339568 220 -1.571428299 10 11 -1 -0.0892732 -1 0.0766077 232 -1.086027384 13 14 -1 -0.0437049 33 1.326289415 15 16 -1 -0.00655455 42 -0.3333332837 17 18 -1 0.131195 -1 0.0156953 223 1.999997973 20 21 -1 -0.00737586 224 375000 22 23 -1 0.150474 -1 -0.0786513 18 0.1249999851 25 26 -1 0.0560821 -1 -0.046557 -1 -0.033165 -1 0.0730743 29 226"
   "1250000 1 26 226 1.187499404 2 21 263 -3.809729815 3 4 -1 -0.0340296 212 1.057764173 5 20 240 -0.5102552176 6 11 191 -0.33741346 7 8 -1 -0.0149493 6 1.749984741 9 10 -1 0.019707 -1 -0.0904977 289 0.5714284778 12 17 220 -0.04166666046 13 16 291 0.9166663885 14 15 -1 -0.0187412 -1 0.0958236 -1 0.00773291 278 0.4781342447 18 19 -1 -0.0391587 -1 0.0213663 -1 -0.0442262 209 0.08247596771 22 25 249 2500000 23 24 -1 -0.0730134 -1 0.0530273 -1 0.0316221 286 -3.633267164 27 28 -1 0.112106 -1 0.0101927 29 84 0.3333332837 1 20 99 0.07387825847 2 17 87 1.249999881 3 12 77 -0.1486852616 4 5 -1 -0.0108156 3 -0.1304506958 6 7 -1 7.19508e-05 16 2000000 8 11 21 -0.9999998212 9 10 -1 -0.0755583 -1 0.0319111 -1 -0.130486 81 0.8571427464 13 14 -1 -0.0144128 233 0.03614541516 15 16 -1 -0.013951 -1 -0.147404 50 -0.4166816473 18 19 -1 -0.0203955 -1 0.0247891 30 -0.07412831485 21 22 -1 0.0330084 305 0.04146851599"
   "23 24 -1 -0.0528688 95 -0.1170089468 25 26 -1 0.0326747 283 -0.8170725703 27 28 -1 0.0190218 -1 -0.0180251 29 316 77 1 28 136 -0.9999996424 2 3 -1 -0.0543346 2 -1.814297318 4 9 18 -0.4999998808 5 6 -1 -0.0953785 307 21 7 8 -1 0.0313946 -1 -0.0635468 305 0.001497944235 10 11 -1 0.0726136 232 -1.601663351 12 13 -1 0.0725232 88 0.3333332837 14 27 276 -1.191608548 15 22 279 -1.331018925 16 21 225 -0.02499999478 17 18 -1 0.0404848 298 -0.3333332837 19 20 -1 -0.0913255 -1 0.00644348 -1 0.0373437 253 0.181845516 23 26 226 -1.424999714 24 25 -1 0.0569749 -1 -0.0516511 -1 0.0170621 -1 0.0328912 -1 -0.0758233 29 80 0.5999999642 1 28 77 -0.4183430374 2 3 -1 0.0204222 92 1.261022687 4 27 271 0.3749998212 5 26 313 3.5 6 15 281 -0.3074240088 7 10 228 -0.9249998331 8 9 -1 0.0958087 -1 -0.0489745 10 -0.0161288511 11 12 -1 -0.00436036 292 0.9749997854 13 14 -1 0.07157 -1 -0.00955099 284 -1.02695775 16 19"
   "259 -0.291664958 17 18 -1 -0.0493224 -1 0.136631 298 0.4062499404 20 23 303 0.230530113 21 22 -1 -0.0611803 -1 -0.00354226 112 0.4285713732 24 25 -1 -0.0126426 -1 0.0602831 -1 -0.0292911 -1 0.0601806 -1 0.0179054 29 55 0.9929063916 1 28 285 0.3958349824 2 11 35 -0.4999999106 3 10 253 0.1611629725 4 5 -1 -0.0313089 5 0.8672202229 6 7 -1 0.0136057 95 0.189528659 8 9 -1 -0.0982667 -1 0.0560052 -1 0.00850942 215 -0.001829004847 12 13 -1 -0.0499177 309 126 14 21 316 53 15 16 -1 0.210144 313 4.5 17 18 -1 -0.05492 267 0.2857142389 19 20 -1 -0.0507037 -1 0.0292375 247 0.7968748808 22 25 284 0.6325677633 23 24 -1 0.0170116 -1 -0.0255633 307 25 26 27 -1 -0.0740205 -1 0.0501419 -1 -0.0188433 29 220 -2.343749523 1 2 -1 0.0785579 70 -0.4503361881 3 6 141 -0.4873656034 4 5 -1 -0.123927 -1 -0.0139354 223 -0.4999999404 7 10 208 -0.662001133 8 9 -1 0.0629979 -1 -0.06133 16 -0.4285713732 11 18 304 0.1565067619"
   "12 13 -1 0.00249602 286 -1.935862184 14 15 -1 -0.163059 316 579 16 17 -1 -0.0178519 -1 -0.11228 117 0.3298264444 19 28 132 0.9999996424 20 27 56 2.172394276 21 24 102 0.02268018387 22 23 -1 -6.02318e-05 -1 0.0188783 245 0.2499999404 25 26 -1 -0.011826 -1 -0.0930351 -1 -0.0167051 -1 0.034055 29 294 0 1 28 244 0.01562499814 2 27 291 -0.1826922894 3 26 276 -1.617049813 4 7 286 -2.734549522 5 6 -1 0.0349095 -1 -0.0320024 277 -1.729301333 8 9 -1 -0.126324 307 47 10 21 307 14 11 18 284 -1.00289166 12 13 -1 0.0306218 234 -1.373815298 14 15 -1 0.0459527 271 0.02083333023 16 17 -1 0.00783928 -1 -0.0989186 305 0.001714284881 19 20 -1 0.0431469 -1 -0.0674778 297 -0.3437499106 22 23 -1 -0.0137431 252 -0.2916666269 24 25 -1 -0.00489471 -1 0.0969838 -1 0.0428245 -1 -0.0477836 -1 -0.069533 29 272 1.583332777 1 28 228 0.9166663885 2 19 156 -0.1176470444 3 4 -1 -0.025001 178 -0.4999998808 5 6 -1 -0.0391351"
   "38 0.07142856717 7 12 130 -0.9999998808 8 9 -1 0.100228 130 -0.09999998659 10 11 -1 -0.0655655 -1 -3.76491e-05 297 0.4999998808 13 14 -1 0.0121048 254 -0.6415323615 15 16 -1 -0.0627105 313 6.5 17 18 -1 0.0368095 -1 0.137219 287 0.3249999285 20 27 273 0.4464285076 21 26 225 0.6562498212 22 23 -1 0.0715795 307 63 24 25 -1 0.312748 -1 -0.046243 -1 0.0329213 -1 -0.0262334 -1 -0.0340766 29 305 0.004354107659 1 8 304 0.006111509632 2 7 297 -0.4374998808 3 4 -1 -0.0407581 298 -0.4999997616 5 6 -1 0.304653 -1 0.0320455 -1 -0.0760311 210 0.8878874779 9 28 26 0.2717887759 10 25 311 1.5 11 16 304 0.008792823181 12 13 -1 -0.0455022 245 -0.9431817532 14 15 -1 0.0548321 -1 -0.00526543 130 -0.6666665673 17 20 138 -0.2033061385 18 19 -1 0.000293566 -1 0.0861686 76 0.6477873325 21 22 -1 0.00663342 208 0.0674591586 23 24 -1 -0.00083701 -1 0.074402 227 -0.1874999702 26 27 -1 0.0243863 -1 -0.00784417 -1 -0.0525918"
   "29 315 0.9938931465 1 28 220 -2.343749523 2 3 -1 0.0886976 146 0.1402929723 4 27 258 -0.03661061078 5 18 275 0.09523807466 6 11 307 3 7 10 240 -0.01668217219 8 9 -1 -0.0729457 -1 0.0332352 -1 0.00880417 255 0.2262663096 12 17 156 1.499999762 13 16 231 -2.091638327 14 15 -1 -0.0639144 -1 0.0246221 -1 -0.0655426 -1 0.0550452 281 -0.8975131512 19 22 304 0.1280677766 20 21 -1 -0.0137179 -1 -0.0811921 277 -0.6820201278 23 24 -1 0.0230297 117 0.3520159721 25 26 -1 -0.00567723 -1 0.0428473 -1 -0.00650025 -1 -0.0376701 29 315 0.8726333976 1 2 -1 -0.0752544 125 2.643723249 3 28 292 -0.2499999553 4 13 289 -0.7291665673 5 12 287 -1.718749523 6 7 -1 0.0657157 1 -1.049059749 8 9 -1 -0.0787768 13 -0.7142856121 10 11 -1 0.052095 -1 -0.0666068 -1 0.0461228 21 -0.3333332241 14 21 270 0.5624998808 15 20 310 60 16 17 -1 -0.101673 174 0.1428571194 18 19 -1 0.0107049 -1 -0.0779029 -1 0.0578587 220 -0.4999999106"
   "22 23 -1 0.0740522 174 -3.249999285 24 25 -1 0.097619 8 -1.355816722 26 27 -1 0.0191125 -1 -0.0589659 -1 -0.0455812 29 239 0.9520554543 1 28 257 0.8692324162 2 27 270 1.424999714 3 20 271 2.562498808 4 19 298 0.7708331943 5 14 284 1.029084802 6 13 167 0.2499981225 7 8 -1 -0.0190006 307 3 9 10 -1 0.0615282 131 -0.1666666418 11 12 -1 -0.0553463 -1 0.00629925 -1 0.112981 287 0.5208332539 15 18 256 0.2102197409 16 17 -1 0.056839 -1 -0.0468145 -1 -0.0484939 -1 -0.0710006 312 2.5 21 22 -1 -0.019785 304 0.4690995514 23 26 7 -0.3191046417 24 25 -1 0.14702 -1 -0.0483672 -1 -0.0411036 -1 0.0344126 -1 -0.0428755 29 304 0.8316112757 1 28 261 -0.8344950676 2 5 227 -0.6041665673 3 4 -1 0.0452637 -1 -0.048367 25 0.2794995904 6 27 314 14.5 7 20 306 0.05138574541 8 13 268 0.2624999881 9 12 218 3375000 10 11 -1 -0.0286169 -1 0.165772 -1 0.0208025 288 0.1874999553 14 15 -1 -0.00452857 228 -0.3035713732 16 17"
   "-1 -0.0609358 267 0.2083332986 18 19 -1 0.070052 -1 0.0206242 266 -0.9999998808 21 22 -1 0.276506 304 0.1856260449 23 26 151 -0.1428571194 24 25 -1 0.00510698 -1 0.0720963 -1 -0.00783786 -1 -0.00409381 -1 -0.0647692 29 312 2.5 1 14 197 -1.199999809 2 3 -1 -0.0248659 278 -0.1246135756 4 7 284 -0.8197471499 5 6 -1 -0.00856416 -1 0.0220745 7 -0.403411299 8 9 -1 -0.0144025 288 0.5714284778 10 13 86 -3.999998093 11 12 -1 -0.157459 -1 0.0219034 -1 -0.0184005 242 1625000 15 28 69 0.4702444673 16 27 94 0.2206184864 17 26 130 -0.2857142687 18 21 132 -0.3333332837 19 20 -1 0.0102419 -1 -0.046013 311 2.5 22 25 2 -0.2269144356 23 24 -1 0.00131031 -1 0.038629 -1 -0.0189103 -1 0.0290009 -1 -0.0270021 -1 0.161942 29 315 0.8726333976 1 2 -1 -0.0748954 115 0.01513232198 3 28 84 1.076923013 4 27 102 -2.068872452 5 6 -1 -0.0491403 290 -0.6874998212 7 8 -1 0.033087 252 -0.4999998808 9 18 234 -0.9461113811 10"
   "17 238 -1.025806665 11 14 14 -0.4999999106 12 13 -1 0.0292721 -1 -0.0490146 293 -0.4999998808 15 16 -1 0.0478188 -1 -0.114559 -1 0.0680603 8 -0.7238342166 19 24 306 0.04073333368 20 23 18 -0.6666662097 21 22 -1 -0.0901934 -1 0.0604607 -1 -0.0191753 11 -1.399999738 25 26 -1 -0.148997 -1 0.00189706 -1 0.0591683 -1 -0.027244 29 114 1.166666508 1 26 303 0.1924695075 2 25 284 0.02137941122 3 8 41 0.4999999106 4 7 310 12 5 6 -1 0.0665375 -1 -0.0601503 -1 0.0160668 237 -0.02760083973 9 20 76 0.2507973015 10 13 305 0.01065593492 11 12 -1 0.00472293 -1 -0.0557215 227 0.624999404 14 19 265 -0.0624999851 15 18 284 0.230050534 16 17 -1 0.327472 -1 0.00870674 -1 0.029095 -1 -0.0513054 286 -0.3804640174 21 22 -1 -0.0128141 250 0.02083333023 23 24 -1 -0.0346163 -1 0.0458785 -1 0.00863866 233 -0.6990063787 27 28 -1 -0.0350749 -1 0.041088 29 256 -1.263305306 1 4 234 -1.432378411 2 3 -1 0.0618757 -1 -0.0471793"
   "270 -0.7499998212 5 6 -1 0.0604239 289 -0.4999999404 7 14 304 0.2293825448 8 11 272 -0.1249999776 9 10 -1 -0.027736 -1 0.0173626 28 -0.524522543 12 13 -1 -0.048841 -1 0.0879364 303 0.08808559924 15 16 -1 0.00288396 244 -0.5937498808 17 18 -1 0.0815683 246 0.7499992251 19 20 -1 -0.0251038 236 -0.484372586 21 24 255 -1.033334494 22 23 -1 0.298974 -1 0.0499233 288 0.4749999046 25 26 -1 -0.0238904 271 0.1749999672 27 28 -1 -0.0601576 -1 0.0634697 29 268 -0.9999998808 1 2 -1 0.0215405 7 -3.146008492 3 4 -1 -0.0409063 308 107 5 6 -1 -0.00248743 257 0.6480680108 7 24 306 0.02901024558 8 15 274 0 9 14 30 1.163384438 10 11 -1 0.0830202 223 0.3749998212 12 13 -1 0.0521444 -1 0.434627 -1 0.0388144 291 0.05208332837 16 19 254 0.6850532889 17 18 -1 0.039184 -1 -0.101177 307 105 20 23 232 0.3109226525 21 22 -1 -0.0470971 -1 0.0383042 -1 0.0377903 298 0.1562499553 25 26 -1 0.0585336 213 1.010412931 27 28"
   "-1 -0.0490336 -1 0.0703656 29 294 0 1 28 244 -0.9249998331 2 3 -1 0.0377643 278 -0.9792333841 4 11 303 0.9692437649 5 8 26 0.4924161136 6 7 -1 -0.0835961 -1 -0.00356797 18 -0.4999998808 9 10 -1 -0.0936925 -1 0.0224514 226 -1.424999714 12 13 -1 0.0955116 295 -1.145833135 14 17 272 0.348837167 15 16 -1 -0.0697105 -1 0.0372216 1 -0.8959590197 18 23 268 -0.4062499404 19 22 304 0.00633456884 20 21 -1 0.0486994 -1 -0.0869545 -1 0.0669677 271 -2.220446049e-16 24 25 -1 -0.0508285 315 0.9407407641 26 27 -1 -0.0553911 -1 0.0214525 -1 -0.0674866 29 100 -1.019443035 1 2 -1 0.0474089 82 0.4999999404 3 16 303 0.1924695075 4 15 232 -0.270124346 5 6 -1 -0.0438225 280 -0.417670548 7 8 -1 -0.073488 235 0.02343671955 9 12 74 -0.2502101958 10 11 -1 0.11919 -1 0.0146017 246 0.2916665673 13 14 -1 0.0228612 -1 -0.0295042 -1 0.00584488 313 6.5 17 20 120 -0.1208097562 18 19 -1 0.0314665 -1 -0.0215797 304 0.1930948198"
   "21 26 247 0.1249999553 22 23 -1 -0.000177965 280 -0.006589761004 24 25 -1 0.23127 -1 0.0701872 80 0.1428571194 27 28 -1 -0.0244368 -1 0.0565032 29 259 1.18749404 1 26 308 268 2 25 249 2500000 3 22 226 1.187499404 4 9 255 1.889405131 5 8 32 1.152417421 6 7 -1 0.00409412 -1 -0.0237215 -1 -0.076296 226 1250000 10 15 209 0.04433612898 11 12 -1 -0.0697737 304 0.009396043606 13 14 -1 0.136796 -1 -0.0185143 260 0.2244466543 16 21 307 155 17 20 289 0.5833331347 18 19 -1 0.053894 -1 -0.0587113 -1 0.141566 -1 -0.125737 314 8.5 23 24 -1 -0.0304178 -1 0.095101 -1 -0.0423438 306 0.2159394473 27 28 -1 -0.0629846 -1 0.0402895 29 122 -0.9494446516 1 8 132 -0.4999998808 2 3 -1 0.0333804 94 0.2969889343 4 7 48 0.02593089454 5 6 -1 0.00687774 -1 -0.0601757 -1 0.013908 7 0.2203412354 9 16 237 0.6549214125 10 13 294 -0.1249999553 11 12 -1 -0.0143754 -1 0.00375521 305 0.9173346162 14 15 -1 -0.0158757 -1 -0.107147"
   "36 0.6666664481 17 22 76 -0.2787005603 18 21 52 0.3999967873 19 20 -1 0.0220029 -1 -0.0775397 -1 0.04695 288 0.624999404 23 24 -1 -0.00225241 13 -0.6666665673 25 26 -1 -0.29649 56 1.916591883 27 28 -1 0.0595075 -1 -0.266762 29 316 77 1 28 310 60 2 3 -1 -0.0479364 310 80 4 7 266 0.4843749404 5 6 -1 0.0604762 -1 -0.036626 310 94 8 9 -1 -0.0617243 249 0 10 27 4 -0.9813854098 11 22 8 -0.9376042485 12 21 286 -2.734549522 13 18 212 -1.23418808 14 17 72 0 15 16 -1 0.0223016 -1 -0.056224 -1 0.0591991 267 0.1249999851 19 20 -1 0.0124996 -1 -0.0854363 -1 -0.0511303 260 -0.9510096312 23 24 -1 -0.0722342 8 -0.6162589788 25 26 -1 0.0691917 -1 0.0110757 -1 -0.0540628 -1 -0.0741773 29 219 -0.3749999106 1 6 248 -0.02499999478 2 5 209 -0.7756491899 3 4 -1 0.0765428 -1 -0.0657459 -1 -0.0439948 14 -0.1666666418 7 8 -1 0.0150079 129 -0.1666666418 9 20 293 0.6562498212 10 11 -1 -0.00861735 25 0.2434236109 12"
   "13 -1 0.011099 3 -0.9971869588 14 15 -1 -0.0123457 305 0.7679823041 16 19 260 0.3460448384 17 18 -1 0.21304 -1 0.0248024 -1 -0.0205897 100 -1.391607046 21 22 -1 0.122541 13 -0.4999999106 23 26 29 0.2857124507 24 25 -1 -0.0259266 -1 0.0396179 36 -2.399999619 27 28 -1 0.0446477 -1 -0.0452965 29 103 -0.9999997616 1 4 80 -0.8333331943 2 3 -1 -0.00633546 -1 0.0240341 220 -0.7499999404 5 8 288 3.249996662 6 7 -1 -0.0420882 -1 0.116769 294 -0.1249999553 9 28 1 -0.6122024655 10 17 257 -0.7807700038 11 12 -1 0.0424245 309 41 13 16 262 0.5482296944 14 15 -1 0.0550372 -1 -0.09393 -1 -0.0484498 303 0.07770858705 18 23 279 -0.241023913 19 20 -1 -0.0212547 227 0.4583331943 21 22 -1 -0.00429058 -1 0.139077 303 0.7549455166 24 27 316 603 25 26 -1 0.0722389 -1 0.295222 -1 -0.0379955 -1 -0.00391339 29 279 -1.052762866 1 14 300 0.5 2 9 259 0.2678556144 3 8 248 0.2499999106 4 7 10 0.2413776219 5 6 -1 -0.0137259"
   "-1 0.0309471 -1 0.0501564 -1 0.0425105 316 100 10 11 -1 -0.191994 259 0.5208315849 12 13 -1 0.0717986 -1 -0.0916964 6 1.333328366 15 26 79 2.892840385 16 25 102 0 17 18 -1 -0.00571271 258 -0.05570685118 19 24 246 0.6249998808 20 21 -1 0.0384693 271 1.374999285 22 23 -1 0.00695742 -1 -0.131838 -1 0.00103192 -1 -0.0253264 260 0.1377613842 27 28 -1 0.0513513 -1 -0.0317417 29 315 0.8726333976 1 2 -1 -0.073745 195 0.1666666418 3 28 82 1.999999285 4 27 193 -0.002007459989 5 6 -1 -0.0129695 51 0.3685485423 7 26 96 0.005152328871 8 21 223 -0.2812499404 9 10 -1 0.0641196 6 -3.999966621 11 12 -1 0.0758596 258 -1.043045402 13 14 -1 -0.0345903 243 -0.5416665673 15 18 314 0.5 16 17 -1 -0.0699235 -1 0.069742 298 -0.6249998808 19 20 -1 -0.0901696 -1 0.00903444 38 0.1249999851 22 25 232 -1.02045691 23 24 -1 -0.126975 -1 -0.0304369 -1 0.0272065 -1 0.0525192 -1 0.0681214 -1 -0.0533203 29 217 0.479398191 1"
   "28 218 0.7343748808 2 27 228 0.9166663885 3 22 295 0.232142821 4 5 -1 -0.0044844 268 0.5340908766 6 21 260 0.06316950917 7 16 307 7 8 11 272 -0.4999999404 9 10 -1 0.183831 -1 0.029912 315 0.9077253342 12 15 309 18 13 14 -1 0.0106384 -1 -0.0595222 -1 0.167641 303 0.04204229638 17 18 -1 -0.00338905 280 -0.417670548 19 20 -1 -0.0127893 -1 0.129328 -1 -0.0655237 273 0.8749995828 23 26 264 0.2678571045 24 25 -1 0.114024 -1 -0.00483047 -1 -0.0466887 -1 -0.0260028 -1 -0.041848 29 44 -0.9999998212 1 4 147 -0.02829698473 2 3 -1 0.00177752 -1 -0.0347871 208 0.8646788597 5 28 10 0.7441816926 6 27 146 0.3221418858 7 14 77 -0.1799068749 8 11 311 1.5 9 10 -1 -0.00476169 -1 0.0236297 15 0.3636363447 12 13 -1 -0.000859254 -1 -0.0346421 245 0.6428570747 15 20 116 0.1300592721 16 19 80 -0.9999998212 17 18 -1 -0.0338738 -1 0.0243102 -1 -0.011275 27 -0.1296161413 21 26 25 0.1398992091 22 25 289 0.2499999553"
   "23 24 -1 0.0625848 -1 0.412896 -1 0.0581789 -1 0.0224831 -1 -0.0306547 -1 -0.0339659 29 134 -0.3749999404 1 6 55 1.109223008 2 5 132 1.499999642 3 4 -1 -0.00581417 -1 -0.0414248 -1 0.0423172 99 0.8760316372 7 16 107 0.9999989867 8 15 3 0.037262097 9 10 -1 0.000560712 197 0.5714284778 11 12 -1 0.0268581 270 1.374999285 13 14 -1 -0.00650795 -1 -0.126675 -1 0.0420634 311 1.5 17 22 23 0.5693098307 18 21 272 -0.1249999851 19 20 -1 -0.0405174 -1 0.0654717 -1 -0.0260759 214 0.4226201773 23 28 295 0.6999998689 24 25 -1 -0.0338492 24 0.3581196368 26 27 -1 -0.0180154 -1 0.240648 -1 -0.0893605 29 294 0 1 28 172 0.5714284778 2 27 107 0.230769217 3 22 161 -0.004921252374 4 5 -1 -0.0174027 222 -0.1022727191 6 21 92 0.01656304114 7 20 276 -1.191608548 8 15 266 0.7708331943 9 14 251 -1.174999714 10 13 14 -0.4999999106 11 12 -1 0.060961 -1 -0.0570607 -1 0.0600296 -1 -0.0609377 8 -1.014009237 16 17 -1 0.0259839"
   "0 -0.9739187956 18 19 -1 -0.0892779 -1 0.00624121 -1 -0.0406437 -1 0.0568174 130 0.1538461447 23 26 27 0.98634547 24 25 -1 0.0736104 -1 -0.0218035 -1 -0.0542147 -1 -0.0533289 -1 -0.0649491 29 267 0.3499999344 1 4 17 0 2 3 -1 -0.00172177 -1 -0.0412261 293 0.312499851 5 14 257 -0.02957301028 6 11 307 14 7 8 -1 -0.042866 51 0.4266933799 9 10 -1 0.0319736 -1 0.19234 223 1000000 12 13 -1 -0.0356851 -1 0.121559 162 -0.5820154548 15 18 218 0.04166665301 16 17 -1 0.282306 -1 -0.0171815 281 0.7922416925 19 26 13 -0.5714284778 20 21 -1 0.0436324 310 3 22 25 2 -0.2269144356 23 24 -1 0.0003008 -1 0.259089 -1 -0.0172814 294 0.8749991059 27 28 -1 0.103134 -1 -0.0107061 29 304 0.7093615532 1 24 283 0.7717392445 2 13 232 0.2368386537 3 12 306 0.01259546727 4 7 130 -0.6666665673 5 6 -1 0.0299165 -1 -0.0292778 25 0.2794995904 8 11 229 -2.220446049e-16 9 10 -1 -0.00998573 -1 0.0205225 -1 -0.00235547 -1 -0.0195933"
   "287 0.3928570747 14 23 303 0.002332385629 15 22 97 -0.02786603197 16 21 94 0.08387231082 17 20 257 0.1586209983 18 19 -1 0.31212 -1 0.0471288 -1 -0.00729196 -1 -0.0362648 -1 -0.0235089 -1 0.067276 309 32 25 26 -1 -0.0565208 313 3.5 27 28 -1 -0.0743452 -1 0.136178 29 220 -1.571428299 1 4 12 -0.9999989867 2 3 -1 -0.0413438 -1 0.0592789 7 -2.535672665 5 8 203 -2.333333015 6 7 -1 0.109408 -1 -0.0288417 2 -1.814297318 9 12 252 0.3928570747 10 11 -1 0.0480806 -1 -0.0064698 2 -0.9088678956 13 22 310 111 14 19 225 0.0624999702 15 16 -1 0.0727017 294 -0.0624999702 17 18 -1 -0.0873064 -1 0.0147462 241 -1.776356839e-15 20 21 -1 -0.0371649 -1 -0.00388128 7 -1.871306777 23 24 -1 0.16268 291 -0.8461537957 25 26 -1 0.0392422 269 -0.1999999583 27 28 -1 0.0307952 -1 -0.0020417 29 316 77 1 28 159 -0.9999998808 2 3 -1 -0.0487887 58 1.799999595 4 27 172 0.08333332837 5 24 56 -1.566627979 6 9 114 0.1428571194"
   "7 8 -1 -0.0668824 -1 0.0346422 247 -0.6666665673 10 11 -1 0.0490954 228 -1.149999738 12 13 -1 -0.0492325 270 -0.3499999344 14 15 -1 0.0555454 270 0.5624998808 16 23 310 125 17 18 -1 -0.0368485 27 0.0008733270806 19 20 -1 0.0438333 313 4.5 21 22 -1 0.0177178 -1 -0.0499121 -1 0.0295076 271 1.749998212 25 26 -1 -0.0523299 -1 0.0733989 -1 0.0626985 -1 -0.0729829 29 316 584 1 22 217 0.479398191 2 17 10 0.6274150014 3 10 315 0.101333335 4 5 -1 0.0591039 218 0.7343748808 6 7 -1 0.00283028 243 1000000 8 9 -1 -0.0462639 -1 0.0734612 2 -0.5995899439 11 12 -1 -0.0218717 278 -0.528416276 13 16 263 -0.2510411441 14 15 -1 -0.0341912 -1 0.369267 -1 0.0477973 238 0.2148117274 18 19 -1 -0.0696078 259 0.3035694659 20 21 -1 -0.047871 -1 0.147894 13 -0.3749999404 23 28 252 0.08333330601 24 25 -1 -0.0562535 242 -0.03124999255 26 27 -1 -0.0599197 -1 0.0714474 -1 -0.0706917 29 210 0.9540166855 1 28 256 -0.6899793148"
   "2 17 207 -0.7500265837 3 16 292 -0.4999994934 4 5 -1 -0.0759573 242 -0.1249999851 6 13 274 0.02777777426 7 10 312 2.5 8 9 -1 -0.0598078 -1 0.0456455 297 0.2812499404 11 12 -1 0.11141 -1 -0.020988 294 0.4999998212 14 15 -1 0.00811213 -1 -0.0583956 -1 -0.0462826 297 1.218749642 18 27 289 0.5937498808 19 22 226 1250000 20 21 -1 0.00303927 -1 0.0697097 3 0.3546503484 23 26 273 -0.3124999106 24 25 -1 0.0737322 -1 -0.0295294 -1 0.0436388 -1 0.0347958 -1 -0.0667918 29 84 0.2727272511 1 26 99 0.07035357505 2 23 169 -0.5016833544 3 14 41 1.333332896 4 11 179 0.9999989867 5 10 117 -0.1191861257 6 7 -1 0.0351556 191 -0.6376807094 8 9 -1 -0.132979 -1 -0.0193021 -1 0.0435291 53 -0.3513489068 12 13 -1 -0.205398 -1 -0.0586544 36 0.2499999404 15 16 -1 0.00757066 23 0.4341812134 17 22 247 0.5468749404 18 19 -1 -0.0110906 38 -0.9999989867 20 21 -1 -0.195661 -1 -0.0390832 -1 0.0127783 50 -0.4166816473 24 25"
   "-1 -0.021703 -1 0.0245638 95 -0.1170089468 27 28 -1 0.0260722 -1 -0.0115108 29 247 0 1 28 310 60 2 3 -1 -0.0495838 310 80 4 7 246 -0.1249999776 5 6 -1 0.0705341 -1 -0.00978713 271 0.9999997616 8 25 310 94 9 10 -1 -0.0753178 172 0.1666666418 11 24 276 -1.617049813 12 19 230 -1.823509574 13 18 18 -0.4999998808 14 15 -1 -0.0478929 17 -0.2499999702 16 17 -1 0.0573013 -1 -0.023865 -1 0.0648163 276 -1.552651763 20 21 -1 -0.0944527 271 0.4249999225 22 23 -1 0.00602168 -1 -0.0490785 -1 -0.0729165 298 -0.09999997914 26 27 -1 0.0724606 -1 -0.0754024 -1 -0.0550614 29 70 -0.4013007283 1 4 138 1.0200001 2 3 -1 0.0492027 -1 0.000640439 305 0.005248539615 5 8 34 0.2499999404 6 7 -1 -0.00159818 -1 0.0516751 49 0.05484793335 9 10 -1 -0.0216114 223 -0.3749996126 11 12 -1 0.0699465 297 0.4821427763 13 28 143 -0.421119839 14 21 211 -0.1908125132 15 16 -1 0.175373 292 0.624999404 17 18 -1 -0.0238346 278 0.180839777"
   "19 20 -1 0.162669 -1 -0.048677 293 0.562499702 22 23 -1 -0.0367034 200 0.1428571194 24 27 203 -1.249999642 25 26 -1 0.0866506 -1 -0.0358642 -1 0.0543693 -1 0.0309849 29 5 1.119276643 1 20 28 -0.9126011133 2 5 290 0.4642856419 3 4 -1 -0.0834408 -1 0.0268788 77 0.505155921 6 19 69 0.378298223 7 18 95 0.1235584617 8 17 33 -0.05444620177 9 14 185 -0.1278379112 10 13 62 1.66666615 11 12 -1 0.0129372 -1 0.12292 -1 -0.0175484 217 -12.49146557 15 16 -1 -0.0393635 -1 0.0123175 -1 -0.0150437 -1 0.0200875 -1 -0.010684 269 250000 21 28 297 1.149999738 22 27 260 0.7723885179 23 26 21 -0.3749999404 24 25 -1 0.0206837 -1 -0.0776802 -1 0.0846324 -1 0.0789564 -1 0.0786716 29 110 -4.666665077 1 6 223 1.916666031 2 5 260 0.168969065 3 4 -1 0.0379644 -1 -0.0733719 -1 -0.082481 0 0.7167333961 7 18 1 0.9391726255 8 15 307 127 9 10 -1 -0.00213102 307 153 11 12 -1 0.0659716 7 -0.7648393512 13 14 -1 -0.0234901 -1"
   "0.0207789 269 0.7812498212 16 17 -1 -0.109659 -1 0.0783134 316 337 19 28 181 -1.249999642 20 25 272 0.1428571194 21 24 188 -0.02298299782 22 23 -1 -0.129081 -1 0.0109162 -1 0.0131718 238 0.07378222793 26 27 -1 -0.0333367 -1 0.0327143 -1 0.0502204 29 316 49 1 26 8 -1.813358903 2 5 308 24 3 4 -1 0.0642461 -1 -0.0428581 235 -1.521043301 6 13 293 -0.4999994934 7 8 -1 -0.0557433 297 -0.7499997616 9 10 -1 0.0452794 291 -0.3333332837 11 12 -1 -0.0456504 -1 0.0525037 247 -0.3999999166 14 15 -1 0.04327 307 47 16 21 268 0.6339285374 17 20 230 -0.8395404816 18 19 -1 -0.0493446 -1 0.016165 -1 0.0590718 225 -0.08333330601 22 23 -1 -0.0695383 248 0 24 25 -1 0.063491 -1 -0.0613459 276 -1.672812462 27 28 -1 0.0453363 -1 -0.0707593 29 231 -1.51170969 1 2 -1 -0.0434883 56 -1.499934793 3 12 79 -1.025630713 4 11 251 -0.2499999106 5 6 -1 0.127765 237 0.03361009806 7 10 256 -0.1677497476 8 9 -1 -0.0491024 -1 0.0704982"
   "-1 0.0934154 -1 -0.00241254 101 -0.686996758 13 14 -1 0.0286187 135 1.499999285 15 22 1 1.151613355 16 21 3 -0.112739712 17 20 106 1.599999666 18 19 -1 -0.00788462 -1 0.0373758 -1 -0.0405143 -1 0.0833301 184 0.4659458995 23 24 -1 -0.0247566 182 -0.2499999702 25 26 -1 -0.0206686 245 -0.147727266 27 28 -1 -0.0162175 -1 0.103842 29 130 -0.3333333135 1 10 132 -0.4999998808 2 5 115 -0.7128589153 3 4 -1 -0.0527986 -1 0.0066804 61 -0.6666664481 6 9 92 0.5043549538 7 8 -1 -0.0289852 -1 0.0742981 -1 0.030532 16 5000000 11 28 15 -0.6153845787 12 17 315 0.09294319898 13 16 23 0.03169004619 14 15 -1 -0.00744319 -1 0.0908185 -1 0.00179032 218 -0.1499999762 18 19 -1 -0.0360623 289 0.7749998569 20 27 197 0.5555555224 21 22 -1 -0.00608018 270 1.999997973 23 26 182 0.2222221941 24 25 -1 -0.0105835 -1 0.0451709 -1 0.147885 -1 -0.0393576 -1 0.0719586 29 130 -0.3333333135 1 18 132 -0.4999998808 2 3 -1 0.0160411"
   "61 -0.444444418 4 15 310 96 5 14 12 0.4999998808 6 9 201 -0.2499999404 7 8 -1 -0.06667 -1 0.0332987 278 0.1239527389 10 11 -1 -0.00237812 220 0.2249999493 12 13 -1 -0.245171 -1 -0.0321241 -1 0.0316062 120 -0.3498753607 16 17 -1 0.0459025 -1 -0.0383587 16 5000000 19 28 145 -0.0003073454427 20 23 129 2.769229889 21 22 -1 0.0129032 -1 -0.058269 122 -0.02556971088 24 25 -1 0.0221483 252 -0.178571403 26 27 -1 -0.018256 -1 -0.000242643 -1 -0.0724144 29 315 0.8726333976 1 2 -1 -0.0716557 128 0.1428571194 3 28 82 1.999999285 4 27 136 -0.9999996424 5 6 -1 -0.0520959 291 -0.3333332837 7 20 113 1.142856956 8 19 22 -0.4999998808 9 14 14 -0.4999999106 10 11 -1 0.00759387 307 13 12 13 -1 -0.014323 -1 -0.0976584 237 -0.353250891 15 18 57 -0.1818181425 16 17 -1 -0.0257516 -1 0.0367479 -1 -0.0499613 -1 -0.0662601 230 -1.08217454 21 22 -1 0.0755318 230 -0.926682651 23 24 -1 -0.0603826 304 0.01799559966 25"
   "26 -1 0.0594995 -1 -0.0529053 -1 0.0733511 -1 -0.0345382 29 288 -0.4999997616 1 2 -1 -0.0448386 294 -0.4374997914 3 4 -1 -0.0671183 303 0.1924695075 5 16 285 -0.3271819949 6 7 -1 -0.052065 101 -0.2406658977 8 13 235 -0.3126875758 9 10 -1 0.0935936 106 1.249999642 11 12 -1 0.00442816 -1 0.0912096 135 1.999999046 14 15 -1 -0.0141639 -1 0.0489924 268 -0.6249999404 17 26 258 1.155133605 18 25 12 -0.2727272511 19 20 -1 0.0149265 273 0.624999404 21 22 -1 -0.0661917 310 224 23 24 -1 -0.0373291 -1 0.0816582 -1 0.119335 310 23 27 28 -1 -0.0978651 -1 0.0272141 29 261 -0.8890727162 1 2 -1 -0.0341569 257 -0.7807700038 3 10 223 0.0624999702 4 9 275 0.3249999285 5 8 0 -0.8226516247 6 7 -1 0.0879529 -1 0.0085265 -1 -0.0764116 -1 -0.0199507 305 0.2235523164 11 28 290 0.4999998212 12 27 215 0.43764624 13 26 227 0.4062499106 14 15 -1 -0.00991827 309 48 16 25 272 0.5833331347 17 24 305 0.0197160095 18 19 -1"
   "-0.0401935 265 0.7222221494 20 23 275 0.2812499404 21 22 -1 0.0354905 -1 0.186122 -1 -0.00605999 -1 -0.0585881 -1 -0.00173934 -1 -0.0546267 -1 -0.0418699 -1 0.00338449 29 279 -1.052762866 1 22 300 0.5 2 19 306 0.9770019054 3 18 281 -2.189039707 4 9 15 -1.142856956 5 6 -1 -0.0950318 278 -1.034691691 7 8 -1 0.054139 -1 -0.106838 227 0.2749999464 10 11 -1 0.00952745 246 -0.2499998808 12 17 275 0.4374997914 13 14 -1 0.0417731 294 0.4999994934 15 16 -1 -0.0339015 -1 -0.303408 -1 0.0601464 -1 -0.121735 213 0.5833289623 20 21 -1 0.0306961 -1 -0.125682 282 -0.3214271963 23 28 278 -1.034691691 24 25 -1 -0.0696777 186 -0.6295425892 26 27 -1 0.0382895 -1 -0.0105497 -1 0.00167123 29 316 46 1 26 8 -1.813358903 2 5 308 24 3 4 -1 0.0618619 -1 -0.0457547 235 -1.521043301 6 15 293 -0.4999994934 7 10 294 -0.7777776718 8 9 -1 0.0517628 -1 -0.0619161 297 -0.7499997616 11 12 -1 0.0421416 291 -0.3333332837 13"
   "14 -1 -0.0427394 -1 0.0475204 247 -0.3999999166 16 17 -1 0.0399393 78 0.3539324403 18 23 27 0.002046684036 19 22 13 -0.5714284778 20 21 -1 0.0533208 -1 -0.0199005 -1 -0.0422985 39 0.111111097 24 25 -1 -0.00362117 -1 0.0905503 276 -1.672812462 27 28 -1 0.0438272 -1 -0.0656973 29 239 0.9520554543 1 28 257 0.8692324162 2 25 312 2.5 3 10 277 -0.6717108488 4 5 -1 -0.057921 305 0.002824444789 6 7 -1 0.0444491 261 0.8654172421 8 9 -1 -0.015991 -1 0.0876713 278 -0.2226434648 11 22 255 0.2262663096 12 13 -1 0.029587 24 0.2228644341 14 15 -1 -0.0445925 222 0.3333333135 16 21 292 -0.01562499814 17 18 -1 -0.0128811 294 0.1249999702 19 20 -1 0.361007 -1 0.0437667 -1 -0.0274341 298 0.4444443882 23 24 -1 -0.0143459 -1 0.0227814 14 -0.2499999404 26 27 -1 0.0608627 -1 0.00455853 -1 -0.0406572 29 256 -0.7891161442 1 12 15 -0.9999998808 2 9 274 2.220446049e-16 3 6 315 0.1219512224 4 5 -1 0.0692134 -1 -0.0514616"
   "268 -0.9999998808 7 8 -1 -0.00820664 -1 0.0747868 273 875000 10 11 -1 -0.039862 -1 0.0857982 297 1.299999714 13 28 274 0.09999997914 14 27 56 -0.1298069358 15 18 132 1.999999523 16 17 -1 -0.0120706 -1 0.0865008 111 -0.9999998212 19 20 -1 0.0492665 87 -0.2222221941 21 22 -1 -0.0133922 306 0.0426315628 23 24 -1 -0.0113727 310 10 25 26 -1 0.147229 -1 0.0250708 -1 -0.00428499 -1 0.0351005 29 243 1000000 1 24 287 0.5833331347 2 5 135 1.249999642 3 4 -1 0.000701798 -1 -0.0185031 283 0.4585500956 6 21 282 -1.499994993 7 10 304 0.09208466858 8 9 -1 0.00612269 -1 -0.144406 273 1.249999642 11 20 315 0.7268965244 12 13 -1 0.032086 256 0.2102197409 14 17 283 -0.7998884916 15 16 -1 0.0485203 -1 -0.0467427 246 1.49999845 18 19 -1 0.0725086 -1 -0.0682641 -1 0.0791175 46 0.3263367712 22 23 -1 -0.00347363 -1 -0.0887358 264 250000 25 28 5 0.2251547426 26 27 -1 -0.10693 -1 0.0204588 -1 0.0545235 29 247 0.1458333135"
   "1 28 182 -0.9999998808 2 3 -1 -0.0448334 253 0.4232804477 4 25 240 -2.101703882 5 20 2 -1.814297318 6 9 7 -2.027758837 7 8 -1 0.00993256 -1 -0.0713826 232 -1.601663351 10 11 -1 0.0796732 230 -1.344449997 12 17 233 -1.333969593 13 16 304 0.0048340098 14 15 -1 0.0420854 -1 -0.0419649 -1 0.0583178 275 -0.2499999702 18 19 -1 -0.0323538 -1 0.0188081 88 0.4999997616 21 22 -1 -0.045625 315 0.9938931465 23 24 -1 -0.0190536 -1 0.116134 283 -0.7223232388 26 27 -1 -0.0582899 -1 0.0533847 -1 -0.0597989 29 83 1.199999809 1 26 98 -0.2222213596 2 9 6 -0.1428563297 3 6 260 -0.8970918655 4 5 -1 -0.0192548 -1 0.0491282 304 0.8740693331 7 8 -1 0.00566722 -1 -0.071417 115 -0.6360406876 10 13 121 -0.428568989 11 12 -1 0.134844 -1 0.0107123 267 0.3928570747 14 17 29 0.1999993026 15 16 -1 -0.0353631 -1 -0.00723513 197 0.4999999404 18 25 228 -0.3124999106 19 22 1 -0.8092688918 20 21 -1 0.108794 -1 -0.0431771 226"
   "0.5416665077 23 24 -1 -0.0344875 -1 0.0320179 -1 0.0302641 30 0.9458668232 27 28 -1 0.033268 -1 -0.0486756 29 261 -0.8344950676 1 4 227 -0.6041665673 2 3 -1 0.0371263 -1 -0.0447761 240 0.5619304776 5 28 248 -0.7692307234 6 7 -1 0.0647132 281 -0.6785567999 8 27 28 0.6444302797 9 26 309 100 10 15 266 0.2499999702 11 14 220 -0.5999997854 12 13 -1 -0.027595 -1 0.0771521 -1 -0.00979415 264 0 16 17 -1 -0.0367257 305 0.01014738902 18 23 236 -2.174984694 19 20 -1 -0.0616029 294 -0.1562499553 21 22 -1 -0.00700245 -1 0.221438 265 1.249999642 24 25 -1 0.00317039 -1 0.114519 -1 -0.0224551 -1 -0.00132207 -1 -0.0359961 29 258 -0.03661061078 1 6 211 -0.02403917164 2 5 98 -0.2499990612 3 4 -1 -0.0221602 -1 0.00468556 -1 0.0275826 290 -0.5833332539 7 8 -1 -0.0539286 225 0.2187499404 9 12 289 -0.2499999553 10 11 -1 -0.0232404 -1 0.017485 310 128 13 24 310 111 14 19 310 95 15 18 256 0.6622774601 16 17 -1 -0.0173654"
   "-1 0.0254277 -1 0.0400228 303 0.1709592044 20 23 316 551 21 22 -1 -0.0320138 -1 0.0857966 -1 -0.104788 282 -1.324989438 25 26 -1 -0.0728889 231 0.7191720605 27 28 -1 0.0181911 -1 -0.058682 29 316 77 1 28 125 2.643723249 2 27 244 -0.9249998331 3 6 294 -0.1666666418 4 5 -1 0.0724576 -1 -0.0141015 129 -0.1428571194 7 8 -1 -0.0275772 22 -0.4999997616 9 18 314 10.5 10 15 310 237 11 12 -1 0.0237379 314 7.5 13 14 -1 -0.0702938 -1 0.0209049 291 -0.2045454383 16 17 -1 -0.0591431 -1 0.0571867 18 -0.6666662097 19 20 -1 -0.0732641 9 -0.9700221419 21 22 -1 0.0486887 246 -0.09999997914 23 24 -1 0.042801 316 1 25 26 -1 0.0760291 -1 -0.0316192 -1 -0.0421512 -1 -0.0701998 29 271 0.3749998212 1 8 89 1.249999642 2 3 -1 0.00129609 188 1.291787744 4 7 62 -0.3333332241 5 6 -1 -0.0660802 -1 0.0540407 -1 -0.107169 289 0.3749999702 9 20 310 95 10 17 180 -1.285714149 11 16 269 0.3749998212 12 15 283 0.1109735742 13"
   "14 -1 0.245823 -1 -0.00176632 -1 0.00212099 -1 -0.0155578 291 -1.333333135 18 19 -1 0.0610928 -1 -0.0521745 7 -0.5435100198 21 26 260 -0.8260483146 22 23 -1 -0.0879269 242 -0.09999997914 24 25 -1 -0.0723314 -1 0.0639537 310 6 27 28 -1 0.172604 -1 -0.0497312 29 35 -0.4999998808 1 26 215 0.009582830593 2 3 -1 -0.0303734 145 1.876086354 4 21 211 -0.4340405464 5 6 -1 -0.130519 248 1.115384459 7 14 5 0.8821790218 8 9 -1 0.00436071 145 1.002503157 10 13 95 0.1815493852 11 12 -1 -0.0800336 -1 0.0230115 -1 0.0969234 171 -1.836218357 15 16 -1 -0.0669954 290 0.3437499702 17 18 -1 -0.011656 236 -0.03124972619 19 20 -1 0.157835 -1 0.044617 145 2.711910486 22 25 275 0.2812499404 23 24 -1 -0.333676 -1 -0.045074 -1 0.0255019 196 -3.499998331 27 28 -1 0.105834 -1 0.00221573 29 277 -0.7751247287 1 28 248 0.3749998808 2 25 188 0.1152476743 3 22 280 -0.6518138647 4 21 309 41 5 6 -1 -0.057377 10 0.4301032126"
   "7 20 279 -0.8395937085 8 19 233 -1.07569325 9 16 289 0.5624998808 10 15 290 -0.1562499553 11 14 259 0.428568989 12 13 -1 -0.0142548 -1 0.0924695 -1 0.0651763 -1 -0.0884327 306 0.9041848183 17 18 -1 -0.023711 -1 -0.125558 -1 0.0685767 -1 0.0440814 -1 0.029966 282 0.5208315849 23 24 -1 -0.00347459 -1 -0.0744872 272 0.2499999106 26 27 -1 0.067028 -1 -0.00115698 -1 -0.00207279 29 294 0 1 28 244 -0.9249998331 2 5 44 0 3 4 -1 -0.00109591 -1 0.0763606 277 -0.5615931153 6 23 233 -0.7236628532 7 18 245 -0.6341462731 8 15 274 0.562499702 9 14 248 -0.6666665673 10 11 -1 0.0563552 240 0 12 13 -1 -0.00694908 -1 -0.0860905 -1 -0.0787108 272 -0.2499999106 16 17 -1 -0.0303716 -1 0.0408353 0 -1.065227628 19 20 -1 0.0406213 216 -1.143892646 21 22 -1 0.0519242 -1 -0.0832361 304 0.008460044861 24 25 -1 0.0650953 92 0.08883587271 26 27 -1 -0.0562909 -1 0.048049 -1 -0.0584271 29 245 -1.549999714 1 2 -1 -0.0533353"
   "245 -1.333333135 3 4 -1 0.080309 271 0.3749998212 5 12 83 0.9999997616 6 7 -1 0.00195332 189 -0.01247599721 8 9 -1 0.0737943 174 -0.1249999851 10 11 -1 -0.0597879 -1 0.032758 289 0.2499999851 13 18 18 -0.4999998808 14 15 -1 0.0667016 310 63 16 17 -1 -0.00418568 -1 -0.0534873 88 0.9999997616 19 26 7 -0.5182180405 20 25 260 -0.8260483146 21 22 -1 -0.0882234 189 0.218944788 23 24 -1 -0.0139784 -1 0.0543177 -1 -0.0467006 313 6.5 27 28 -1 0.0111516 -1 0.118233 29 213 1.499988794 1 26 220 -1.124999762 2 5 291 -2.593749285 3 4 -1 0.104714 -1 -0.0664805 78 0.3715700507 6 25 69 0.08211119473 7 24 202 -0.9999995232 8 9 -1 0.0205841 222 -0.0624999851 10 17 98 0.2222212404 11 16 306 0.0700616017 12 15 271 0.5833332539 13 14 -1 -0.0428482 -1 0.00262736 -1 0.00964088 -1 0.0271283 189 -0.6050889492 18 21 314 9.5 19 20 -1 0.0558137 -1 -0.026283 74 -0.2432431132 22 23 -1 0.0103889 -1 -0.0279478 -1 0.013469"
   "-1 -0.00792334 15 -1.22222209 27 28 -1 0.0515864 -1 -0.0904713 29 315 0.9938931465 1 28 220 -1.571428299 2 7 280 0.6170477271 3 6 242 -0.9999995232 4 5 -1 -0.0973975 -1 0.0726755 -1 -0.0411657 201 0.4999998808 8 13 289 -0.4999999404 9 12 256 -1.198042989 10 11 -1 0.04643 -1 -0.0148662 -1 0.00490757 168 -0.6640093923 14 21 42 -0.1999999583 15 20 4 -0.1112944558 16 17 -1 0.00484744 253 0.5448379517 18 19 -1 -0.184347 -1 -0.00222857 -1 -0.021736 104 1.999999642 22 27 46 0.4404323399 23 24 -1 0.00551599 208 0.008351461962 25 26 -1 0.00129132 -1 -0.0429219 -1 -0.0721093 -1 -0.0347114 29 247 0.1458333135 1 28 310 60 2 3 -1 -0.0485535 310 80 4 7 246 -0.1249999776 5 6 -1 0.0621742 -1 -0.0121269 310 95 8 9 -1 -0.0498587 253 0.3875407875 10 27 262 -0.3288199902 11 22 212 -1.23418808 12 17 165 -0.001144059002 13 14 -1 -0.0525828 215 -1.211319208 15 16 -1 0.0414941 -1 -0.0307383 8 -0.9542816877 18 19"
   "-1 0.0562222 223 1.249998808 20 21 -1 0.0136771 -1 -0.0931667 285 -1.299608827 23 24 -1 0.0320237 242 -0.6666664481 25 26 -1 0.0238753 -1 -0.0688895 -1 0.0738531 -1 -0.0571183 29 104 0.9999997616 1 20 92 0.002522061346 2 19 245 0.2499999702 3 12 48 0.01776134782 4 7 269 -0.1249999776 5 6 -1 -0.0518796 -1 -0.00472269 144 0.3333318532 8 11 276 0.4051314592 9 10 -1 -0.0219496 -1 0.0436588 -1 0.0572745 306 0.01406094898 13 14 -1 -0.0230507 239 -0.5146360993 15 16 -1 -0.0844236 3 -0.1391578615 17 18 -1 0.0755016 -1 0.00525264 -1 -0.0121251 58 0.7499998212 21 26 157 -0.7142856121 22 23 -1 0.0455442 274 0.3749999404 24 25 -1 -0.0542499 -1 0.0249882 205 0.7499998212 27 28 -1 0.0211287 -1 0.126945 29 304 0.8536652923 1 28 210 0.9540166855 2 27 283 0.7717392445 3 16 232 0.2368386537 4 13 306 0.01259546727 5 12 247 0.4374997914 6 7 -1 -0.0395226 223 1.999997973 8 9 -1 -0.0168632 296 -0.0624999702 10"
   "11 -1 0.275245 -1 0.0266419 -1 0.00400836 311 2.5 14 15 -1 -0.032245 -1 0.00913665 238 0.4234411418 17 24 262 0.07049655914 18 21 97 -0.2443947196 19 20 -1 -0.0859351 -1 0.0591712 315 0.8103864789 22 23 -1 -0.0750891 -1 0.0502169 25 -0.08042955399 25 26 -1 -0.0376269 -1 0.103519 -1 -0.0625629 -1 -0.0700477 29 7 -2.830753565 1 4 242 1625000 2 3 -1 -0.0287105 -1 0.131954 260 -1.723551869 5 6 -1 0.0474472 285 0.3958349824 7 24 276 0.2233695388 8 13 279 0.236622408 9 10 -1 -0.00158114 5 0.78360641 11 12 -1 0.0243828 -1 0.110299 111 -1.481481314 14 15 -1 -0.0584292 263 0.2655400634 16 19 263 0.02055101655 17 18 -1 -0.0115961 -1 0.0361895 305 0.977494061 20 21 -1 -0.0447251 143 0.1706757396 22 23 -1 0.00135732 -1 -0.523951 267 0.3928570747 25 26 -1 0.0244378 22 -0.8333331943 27 28 -1 0.160161 -1 -0.00835028 29 316 46 1 26 8 -1.813358903 2 3 -1 0.0387736 235 -1.521043301 4 7 293 -0.4999994934 5"
   "6 -1 -0.0456243 -1 0.000106959 247 -0.3999999166 8 9 -1 0.0359027 307 47 10 21 268 0.616666615 11 20 8 -0.9724505544 12 15 307 14 13 14 -1 0.0602639 -1 -0.02799 41 -0.4999998808 16 17 -1 0.0773752 9 -1.335283279 18 19 -1 0.0555351 -1 -0.0622955 -1 0.0522681 225 -0.08333330601 22 23 -1 -0.0654719 248 0 24 25 -1 0.0581332 -1 -0.0573804 241 -1.349999785 27 28 -1 0.0354604 -1 -0.0650492 29 268 0.5999999642 1 24 254 1.107005477 2 23 271 125000 3 22 298 0.8124995828 4 17 239 0.6598617435 5 14 78 -0.6782774329 6 7 -1 0.030169 42 -0.9999998212 8 9 -1 0.0336386 16 0 10 11 -1 -0.000410852 210 0.8415645957 12 13 -1 -0.0419577 -1 0.164165 242 0.7222220898 15 16 -1 -0.0595959 -1 0.0130016 291 0.0624999851 18 21 13 -1.333333135 19 20 -1 0.0811191 -1 -0.0270158 -1 0.0482724 -1 -0.060167 -1 0.0669712 255 1.381267309 25 26 -1 -0.0589791 305 0.2382183671 27 28 -1 -0.063345 -1 0.146396 29 240 0.9012978673 1"
   "28 308 248 2 21 259 1.18749404 3 20 190 -0.285712868 4 5 -1 0.00847164 198 -0.4999997616 6 9 87 1.249999881 7 8 -1 0.00539561 -1 0.0580666 188 0.2947493792 10 13 28 0.4690092802 11 12 -1 -0.00700987 -1 -0.0373084 306 0.07804760337 14 17 130 -1.124999881 15 16 -1 0.0937258 -1 -0.0166277 78 -0.6256093383 18 19 -1 -0.0478328 -1 0.030553 -1 -0.0399553 315 0.6188016534 22 27 199 1.999999642 23 26 222 0.5781249404 24 25 -1 -0.0707543 -1 0.0740599 -1 0.0538578 -1 0.0378363 -1 -0.0826157 29 122 -0.8557007313 1 16 94 0.1652799547 2 11 99 -0.4145659506 3 6 196 -2.999998569 4 5 -1 -0.162721 -1 0.00362242 26 0.1819695681 7 10 57 0.6666664481 8 9 -1 -0.144818 -1 -0.00265029 -1 -0.0340517 311 0.5 12 13 -1 -0.0662592 311 1.5 14 15 -1 0.0826176 -1 0.0112494 7 0.2147876173 17 18 -1 -0.000347963 254 0.1393887252 19 20 -1 0.047713 13 -0.6999999285 21 26 297 0.09374997765 22 23 -1 -0.00547697 303 0.005634368863"
   "24 25 -1 -0.299616 -1 -0.0395093 310 214 27 28 -1 0.0208376 -1 -0.090323 29 195 0.1666666418 1 28 122 0.006436334457 2 25 172 0.5714284778 3 24 107 0.1666666567 4 23 161 -0.004921252374 5 12 212 -0.8648831844 6 7 -1 -0.0439162 267 0.2083332986 8 11 285 -0.8362583518 9 10 -1 0.0543916 -1 -0.027076 -1 -0.0289184 22 -0.4999998808 13 16 232 -0.3297497928 14 15 -1 -0.0151664 -1 0.0575372 235 -0.7908840775 17 18 -1 0.0505815 226 -0.708333075 19 20 -1 -0.115259 315 0.9407407641 21 22 -1 -0.0725862 -1 0.0495395 -1 0.0409071 -1 -0.0640688 292 -0.3749999106 26 27 -1 0.0272438 -1 -0.0567841 -1 -0.0551465 29 272 1.583332777 1 22 228 0.8906248808 2 11 268 0.5999999642 3 10 18 0.2499999404 4 7 47 0.1084024012 5 6 -1 -0.0020397 -1 0.02088 47 -0.2472555488 8 9 -1 0.0221679 -1 -0.0409318 -1 -0.0474371 273 0.7499998808 12 21 1 -0.7709434032 13 16 258 -0.6091946959 14 15 -1 0.0907993 -1 -0.0632953 251 0.1874999553"
   "17 18 -1 0.172548 244 0.2249999493 19 20 -1 -0.0526513 -1 0.0852033 -1 -0.0280713 226 1.999999046 23 24 -1 -0.0937132 316 300 25 28 8 -0.6729235053 26 27 -1 0.0938711 -1 -0.0162316 -1 -0.0651755 29 103 -1.124999881 1 6 307 56 2 5 211 0.2851892412 3 4 -1 0.0101178 -1 -0.0428739 -1 0.0255322 220 -0.178571403 7 12 266 0.1964285374 8 11 291 -2.593749285 9 10 -1 0.0858717 -1 -0.0117477 -1 -0.0747689 283 -1.28949964 13 14 -1 -0.0362863 122 1.38023591 15 28 228 0.04166666046 16 19 289 0.02777777426 17 18 -1 -0.0019048 -1 0.031959 237 -0.7957729697 20 21 -1 0.0203566 248 1.115384459 22 27 237 -0.6672365665 23 24 -1 -0.0425562 290 -0.4999999404 25 26 -1 0.0632205 -1 -0.00879569 -1 0.0187793 -1 -0.0402405 29 70 -0.4503361881 1 6 37 -1.142856956 2 3 -1 0.0490101 141 -0.4873656034 4 5 -1 -0.121133 -1 -0.0190579 223 -0.5999999046 7 8 -1 0.0449037 16 -0.4285713732 9 18 304 0.05078355223 10 11 -1 0.0157104"
   "260 -0.8347268701 12 15 227 0.1249999776 13 14 -1 0.0130632 -1 -0.134109 226 0.08928570151 16 17 -1 -0.0435634 -1 0.00742141 308 109 19 22 269 -0.1249999776 20 21 -1 0.0264946 -1 -0.00151889 258 0.9662328362 23 26 1 -1.408015847 24 25 -1 -0.0444 -1 0.0256919 293 0.2999999523 27 28 -1 0.0689728 -1 -0.0708139 29 315 0.8726333976 1 2 -1 -0.0676638 128 0.1428571194 3 28 4 -0.9813854098 4 23 211 -1.304225922 5 10 315 0.9773463011 6 7 -1 -0.0336811 252 -0.7499998212 8 9 -1 -0.0255657 -1 0.0573544 291 -0.1874999702 11 22 10 -10.32578468 12 15 314 6.5 13 14 -1 0.067259 -1 -0.0262964 296 -0.6249999404 16 21 283 -0.738853097 17 20 207 -1.134630561 18 19 -1 0.0441477 -1 -0.0664863 -1 0.0435966 -1 -0.0514512 -1 0.0437137 126 0.4999999404 24 27 8 -0.6729235053 25 26 -1 0.0523426 -1 0.00828256 -1 -0.0429847 -1 -0.0315643 29 80 0.5999999642 1 28 271 0.3749998212 2 15 278 1.031568527 3 14 13 -0.5263157487"
   "4 7 225 -0.2499999702 5 6 -1 -0.0162223 -1 0.0256835 51 -0.3491337597 8 11 140 -0.3419366181 9 10 -1 0.259001 -1 0.0135252 258 -0.1672187895 12 13 -1 -0.0458653 -1 -0.00288079 -1 0.0691295 146 0.2209817022 16 19 134 -1.249999642 17 18 -1 0.0250105 -1 -0.0485351 310 13 20 21 -1 0.14288 59 1.285714149 22 25 90 -1.999999046 23 24 -1 0.110235 -1 -0.0256548 196 0.3333332837 26 27 -1 0.128481 -1 -0.0215671 -1 0.0143689 29 256 -0.6899793148 1 26 0 -0.7198616862 2 25 316 504 3 14 235 -1.747897863 4 9 271 0.166666612 5 6 -1 -0.0123658 207 -1.386369586 7 8 -1 0.193287 -1 -0.0553998 306 0.4628044665 10 11 -1 -0.0466897 207 -1.01117456 12 13 -1 -0.0697896 -1 0.0527286 233 -1.055991173 15 18 267 -0.3333332241 16 17 -1 0.0563253 -1 -0.0714491 303 0.2900222838 19 22 61 0.2857142389 20 21 -1 -0.0133942 -1 0.10253 303 0.5946635604 23 24 -1 0.146278 -1 -0.0545258 -1 -0.04595 16 -0.4999999106 27 28 -1 0.02715"
   "-1 0.000373017 29 277 -0.7751247287 1 12 235 0.6025767922 2 11 304 0.2293825448 3 4 -1 0.0109429 258 0.03949327022 5 6 -1 0.01205 240 -0.4878827035 7 10 262 0.5482296944 8 9 -1 -0.183157 -1 -0.0338935 -1 -0.0127109 -1 0.0716126 304 0.2704432011 13 14 -1 -0.00587808 305 0.6577096581 15 28 255 0.1758615524 16 25 236 -0.5178541541 17 20 241 0.04166666046 18 19 -1 -0.0532665 -1 0.00709979 199 -0.6666664481 21 24 287 8.881784197e-16 22 23 -1 -0.223721 -1 0.0260309 -1 0.0087977 303 0.04204229638 26 27 -1 -0.0125325 -1 0.0547521 -1 0.141317 29 247 0.1458333135 1 28 310 60 2 3 -1 -0.0445624 253 0.4631983936 4 23 146 0 5 20 244 -0.9249998331 6 9 46 -0.001103181508 7 8 -1 -0.0369462 -1 0.0677425 252 -0.4374999404 10 19 237 -0.09512525052 11 18 307 20 12 15 267 0.09999997914 13 14 -1 0.0282863 -1 -0.0395787 307 95 16 17 -1 -0.0651898 -1 0.0397158 -1 0.0573492 -1 0.0127021 73 -0.1741589457 21 22 -1 0.0434591"
   "-1 -0.0501091 209 -0.06675416976 24 25 -1 0.0775107 193 -0.5248696208 26 27 -1 -0.0384819 -1 0.0503457 -1 -0.0538367 29 239 0.9520554543 1 28 280 0.7497546077 2 19 220 -1.999999523 3 4 -1 -0.0835482 304 0.01431206148 5 8 269 -0.0624999702 6 7 -1 0.0607808 -1 -0.0738891 138 -1.460442066 9 10 -1 0.0903609 233 -0.9242293239 11 12 -1 -0.0256006 283 -0.6993252039 13 18 315 0.6148409843 14 17 305 0.3061870337 15 16 -1 0.134538 -1 -0.0610683 -1 -0.0604928 -1 0.00173228 283 0.6406926513 20 27 289 -0.02499999665 21 22 -1 -0.0450432 21 -0.4999997616 23 24 -1 0.096417 291 0.03124999255 25 26 -1 -0.0286594 -1 0.0600047 -1 -0.0871273 -1 -0.0392877 29 256 0.6352572441 1 14 291 0.6666666269 2 3 -1 0.000160806 27 -0.449237287 4 11 207 -0.06442555785 5 6 -1 0.00444519 25 0.07640779018 7 10 298 0.5799999237 8 9 -1 0.45004 -1 -0.00900719 -1 0.0719807 230 0.1933317333 12 13 -1 -0.0143715 -1 0.0561863 275 1.437499642"
   "15 28 259 -0.3958310187 16 17 -1 0.0300371 236 0.3571405709 18 21 256 0.7888720036 19 20 -1 -0.0874645 -1 -0.0246337 276 -0.55147475 22 23 -1 0.127411 274 -0.01785713993 24 25 -1 0.0773151 240 -1.359065771 26 27 -1 0.0579614 -1 -0.0671203 -1 0.0804824 29 316 3 1 2 -1 -0.0402838 84 0.2727272511 3 28 53 0.06921435148 4 27 305 0.5271471739 5 16 310 117 6 15 315 0.2434988171 7 14 251 0.5416665077 8 13 240 -0.1785690635 9 12 245 -0.7884615064 10 11 -1 0.068829 -1 -0.125605 -1 0.01492 -1 0.0818042 -1 0.0310732 -1 -0.00290197 261 -0.1322496831 17 18 -1 0.0339928 304 0.05516370013 19 26 13 -0.5555555224 20 25 36 0.6666665673 21 22 -1 0.0316375 266 0.1249999776 23 24 -1 -0.029389 -1 -0.224211 -1 0.000622066 -1 -0.0429867 -1 0.0146431 -1 -0.00667565 29 316 49 1 26 274 -0.6718749404 2 3 -1 -0.0249015 220 -0.5249999166 4 7 251 -0.4687498808 5 6 -1 0.0163311 -1 0.0758288 266 -0.2749999464 8 9 -1 -0.0545727"
   "246 -0.08333330601 10 17 142 -0.6906071901 11 12 -1 -0.0233244 305 0.1506130099 13 16 285 -1.151390791 14 15 -1 0.0457616 -1 -0.0260938 -1 0.083113 13 -0.09090908617 18 21 25 -0.0004345616035 19 20 -1 -0.0771306 -1 0.0192101 307 94 22 25 35 -1.124999881 23 24 -1 0.0354787 -1 -0.0706054 -1 0.0273849 276 -1.672812462 27 28 -1 0.041017 -1 -0.0661627 29 33 6.870182037 1 22 225 2.999999046 2 21 245 -1.549999714 3 4 -1 -0.0672029 92 0.5348552465 5 18 92 -0.0001354337874 6 13 294 1.187499404 7 12 176 -0.8571427464 8 11 269 0.1249999404 9 10 -1 0.175671 -1 -0.0143345 -1 -0.00243415 -1 0.0421914 170 -0.06198590994 14 15 -1 0.00996711 205 1.399999738 16 17 -1 -0.0310159 -1 0.0180913 89 0.2857142091 19 20 -1 -0.0139296 -1 0.0530943 -1 -0.0586958 307 24 23 26 298 0.6874998212 24 25 -1 -0.0602998 -1 0.0639575 223 -0.1249999404 27 28 -1 -0.0506428 -1 0.0651694 29 95 -0.2839443386 1 6 258 0.2609511912 2"
   "5 199 -0.7272726893 3 4 -1 0.134993 -1 -0.00968399 -1 -0.0504427 309 32 7 12 262 0.5482296944 8 11 15 -0.4999999404 9 10 -1 0.0135687 -1 -0.0351153 -1 -0.0704423 309 39 13 16 307 6 14 15 -1 0.152646 -1 0.0283424 309 64 17 18 -1 -0.029746 309 84 19 28 307 11 20 27 266 0.09374997765 21 26 280 0.0562097244 22 25 276 0.06715539843 23 24 -1 0.158788 -1 0.512952 -1 -0.0379009 -1 0.0188905 -1 0.0213586 -1 -0.000386166 29 307 1 1 8 24 0.54829669 2 3 -1 -0.0381547 188 0.1503810883 4 7 298 -0.1249999404 5 6 -1 -0.0177229 -1 0.0573549 -1 -0.0602562 312 2.5 9 10 -1 0.00518247 295 0.5208332539 11 22 185 0.4137400091 12 19 151 0.2499999702 13 14 -1 -0.00693373 311 1.5 15 16 -1 -0.00928951 174 0.9999997616 17 18 -1 -0.0379043 -1 -0.115639 232 0.4184323549 20 21 -1 -0.000821052 -1 0.0562184 48 0.4467442632 23 24 -1 0.00113234 42 0.2499999702 25 28 315 0.5382745862 26 27 -1 0.168866 -1 0.0485641 -1 -0.0245297"
   "29 115 0.01513232198 1 28 146 0 2 27 84 1.076923013 3 26 197 0.1818181723 4 25 287 -1.053571224 5 14 270 0.2499998808 6 13 287 -1.312499642 7 12 240 -36.6999321 8 9 -1 -0.0504118 151 -0.1999999732 10 11 -1 -0.033388 -1 0.0708985 -1 -0.0539161 -1 -0.0639635 304 0.004591601901 15 18 316 23 16 17 -1 0.0450909 -1 -0.0306573 266 -0.2857142389 19 20 -1 -0.062097 222 -0.6153845787 21 22 -1 -0.0335494 234 -2.413861036 23 24 -1 -0.0343501 -1 0.036395 -1 -0.0662369 -1 0.0585687 -1 -0.040127 -1 -0.0287678 29 286 -15855.91113 1 2 -1 -0.0803257 33 6.870182037 3 22 316 543 4 19 10 -0.09868358821 5 10 236 -0.2656243443 6 9 209 -0.4614008963 7 8 -1 0.0376659 -1 -0.0294489 -1 -0.0224108 284 -0.2302854806 11 18 306 0.1057571247 12 17 244 0.1249999702 13 16 246 0.09999991953 14 15 -1 -0.0118718 -1 0.0706246 -1 -0.0478868 -1 -0.0556545 -1 0.016903 310 79 20 21 -1 0.0295044 -1 -0.0443398 192 -1.192256212 23 24"
   "-1 0.167692 138 -0.00158290111 25 26 -1 -0.0728194 287 -0.3472221792 27 28 -1 -0.0569671 -1 0.0490559 29 82 1.999999285 1 28 213 1.499988794 2 25 220 -1.124999762 3 6 308 97 4 5 -1 -0.0698307 -1 0.0510395 256 0.6352572441 7 18 268 0.6339285374 8 9 -1 0.00154546 5 0.5960832834 10 17 277 -0.8796935678 11 12 -1 -0.0749266 227 0.03571427986 13 14 -1 0.056015 274 0.3749998808 15 16 -1 0.0260736 -1 -0.050765 -1 0.0951569 275 1.624999166 19 24 315 0.7946057916 20 23 221 0.3035713732 21 22 -1 -0.00287454 -1 -0.0666816 -1 0.0117772 -1 0.0947212 15 -1.22222209 26 27 -1 0.042107 -1 -0.0863088 -1 -0.0285909 29 7 -2.535672665 1 8 203 -2.333333015 2 3 -1 0.100604 217 0.3354812562 4 7 286 -10057.47168 5 6 -1 0.061801 -1 -0.0362407 -1 0.103769 260 -1.723551869 9 10 -1 0.0589822 38 1.363636255 11 18 99 0.8357346654 12 13 -1 0.00100275 116 0.0654315874 14 17 185 0.03756197169 15 16 -1 -0.0100118 -1 -0.0695459"
   "-1 0.0296938 196 -0.4999997616 19 24 253 0.6554313302 20 23 246 1.999997973 21 22 -1 0.0725737 -1 0.242368 -1 -0.0485407 313 5.5 25 26 -1 0.0266625 295 0.6041665673 27 28 -1 -0.0532449 -1 0.0935496 29 294 0 1 28 100 -0.1708313525 2 3 -1 -0.0316097 120 0.000845861272 4 27 291 -0.2187499404 5 24 276 -1.617049813 6 9 119 -0.3666301668 7 8 -1 -0.035048 -1 0.0351441 1 -1.770617485 10 11 -1 -0.116973 307 59 12 21 303 0.9942247868 13 20 307 7 14 19 221 0.0624999851 15 18 297 -0.5624998808 16 17 -1 0.0639251 -1 -0.0243503 -1 -0.032881 -1 -0.0383452 -1 0.0634625 235 -1.521043301 22 23 -1 -0.0162049 -1 0.0599975 250 -0.1666666418 25 26 -1 0.0545592 -1 -0.0313645 -1 -0.0315708 -1 -0.0523952 29 17 -0.3478260338 1 6 189 -0.184816286 2 5 228 0.8906248808 3 4 -1 0.0299968 -1 0.169524 -1 0.00305871 3 -1.847469211 7 8 -1 -0.0415746 245 -1.249999762 9 10 -1 0.101878 263 -1.373591185 11 18 231 -0.008051415905"
   "12 17 290 0.6749998927 13 16 237 -0.8953227997 14 15 -1 0.06532 -1 -0.0449529 -1 0.0638724 -1 -0.0653255 194 3.936490536 19 28 139 -0.0002081083076 20 25 91 1.166666508 21 22 -1 -0.0365503 81 0.5999999046 23 24 -1 -0.0386246 -1 0.100874 56 -1.368399739 26 27 -1 0.0453779 -1 -0.000252566 -1 0.0563857 29 248 -0.7692307234 1 4 266 0.2031249702 2 3 -1 -0.00793054 -1 0.103558 304 0.001452166704 5 6 -1 -0.0555912 269 -0.2499998808 7 18 316 575 8 11 252 -0.3749998212 9 10 -1 0.0323135 -1 -0.0572445 298 0.2812499404 12 15 226 0.01785713993 13 14 -1 0.0344543 -1 -0.088895 265 -0.1249999404 16 17 -1 0.161988 -1 -0.0221217 16 -0.4999999106 19 22 16 -0.5714284778 20 21 -1 0.00650877 -1 0.08227 224 -0.7291665673 23 24 -1 0.0482312 309 32 25 26 -1 -0.017412 309 45 27 28 -1 0.0359373 -1 -0.000908561 29 41 1.124999881 1 4 110 -4.666665077 2 3 -1 -0.0367964 -1 0.00202599 76 0.0002589050564 5 12 229 0.3392856717"
   "6 7 -1 -0.00351507 250 0.5416665673 8 11 196 -1.999999523 9 10 -1 -0.229715 -1 -0.0595362 -1 -0.00533039 186 0.294642508 13 16 185 0.06853043288 14 15 -1 0.0088957 -1 -0.0589551 296 0.5416665077 17 28 135 0.2857142389 18 27 298 0.08333330601 19 20 -1 0.00316842 90 0.9999997616 21 26 253 0.1402688026 22 23 -1 0.0306128 164 0.1487070769 24 25 -1 0.0673186 -1 0.314007 -1 -0.0128698 -1 -0.00197785 -1 -0.02164 29 316 46 1 26 159 -0.9999998808 2 3 -1 -0.0424089 74 1.021190882 4 25 274 -0.6718749404 5 6 -1 -0.0253395 305 0.001497944235 7 8 -1 0.0652947 244 -0.9249998331 9 12 283 -1.070389748 10 11 -1 -0.0321345 -1 0.0732192 313 2.5 13 16 269 0.9374995232 14 15 -1 0.0275163 -1 -0.0321869 256 -0.1677497476 17 20 50 0.005054386798 18 19 -1 0.00431859 -1 -0.0590736 162 -0.2754183412 21 22 -1 -0.0413312 287 -1.468749642 23 24 -1 -0.0596107 -1 0.0435343 -1 0.0590655 276 -1.672812462 27 28 -1 0.0390284"
   "-1 -0.0593473 29 204 1.124999881 1 4 236 -0.2499985993 2 3 -1 0.00721896 -1 -0.00912574 293 0.4722221196 5 18 283 -0.7615751624 6 11 293 -0.08333330601 7 10 266 -0.2749999464 8 9 -1 -0.0346066 -1 0.227857 -1 -0.0457795 48 -0.8647624254 12 13 -1 0.0885058 169 -1.201981783 14 15 -1 0.116745 100 0.8577346206 16 17 -1 -0.0513529 -1 0.0443427 276 0.2233695388 19 22 193 0.9005298018 20 21 -1 0.0361121 -1 -0.0750844 281 0.08022204787 23 24 -1 0.127905 38 0.1428571194 25 28 288 0.4249999225 26 27 -1 0.0568328 -1 -0.0259195 -1 0.0790388 29 226 1250000 1 22 226 1.187499404 2 19 263 -3.809729815 3 4 -1 -0.0291431 212 1.057764173 5 16 240 -0.5102552176 6 11 191 -0.33741346 7 8 -1 -0.0147966 6 1.749984741 9 10 -1 0.0166341 -1 -0.0851564 282 1.499991536 12 13 -1 -0.00354762 266 -0.1249999776 14 15 -1 -0.0436466 -1 0.118665 274 -0.1249999776 17 18 -1 0.0767298 -1 -0.0513489 18 -0.2499999404 20 21 -1 -0.0990482"
   "-1 -0.0198502 289 0.5833331347 23 26 10 0.207403928 24 25 -1 0.0792081 -1 -0.0887486 238 0.9575681686 27 28 -1 -0.0433268 -1 0.100997 29 62 -5.999997139 1 14 271 0.5499998927 2 7 198 -0.3999999166 3 4 -1 0.0653825 77 1.060451031 5 6 -1 -0.023139 -1 0.15331 290 0.3392856717 8 11 8 0.7031646371 9 10 -1 -0.0290372 -1 0.0718789 218 0.4861110449 12 13 -1 -0.0912074 -1 0.0291674 18 0.2499999404 15 20 315 0.1666666716 16 17 -1 -0.0182839 53 -0.3399370313 18 19 -1 -0.024846 -1 0.00260079 202 -1.249999642 21 24 86 1.999999046 22 23 -1 -0.016368 -1 0.0696673 111 -0.9999998212 25 28 280 0.03090253472 26 27 -1 -0.0454293 -1 0.0132788 -1 0.0189403 29 195 0.1666666418 1 28 16 0.7142856121 2 25 306 0.8340455294 3 24 56 -1.566627979 4 7 114 0.1428571194 5 6 -1 -0.0584813 -1 0.0278698 107 0.230769217 8 23 246 0 9 22 287 -1.124999762 10 15 287 -1.312499642 11 14 274 -0.02499999478 12 13 -1 0.0420714 -1 -0.0466575"
   "-1 -0.0593899 316 25 16 21 232 -0.6473751068 17 18 -1 0.0394775 296 -0.4999998808 19 20 -1 0.0391958 -1 -0.0392505 -1 -0.0335532 -1 -0.055163 -1 0.0403685 -1 -0.0512765 252 -0.8124998808 26 27 -1 0.038242 -1 -0.062125 -1 -0.0501116 29 231 -1.51170969 1 2 -1 -0.0409048 170 -0.1598264575 3 20 248 -0.1428571194 4 7 204 0.4999997616 5 6 -1 0.0322655 -1 0.161555 249 -0.08333332092 8 13 295 0.2499999553 9 12 164 -0.4852553308 10 11 -1 0.111751 -1 0.00542586 -1 0.0882596 197 1.333332896 14 19 139 0.4056808949 15 18 105 -0.4999997616 16 17 -1 0.0103707 -1 -0.0578833 -1 0.0123883 -1 0.055325 131 -0.1428571194 21 22 -1 -0.0301633 95 -0.2391516268 23 28 272 -0.9999998808 24 25 -1 -0.030906 297 0.6666664481 26 27 -1 0.0474597 -1 -0.0406275 -1 -0.00514043 29 240 0.9012978673 1 28 0 0.7167333961 2 17 224 -0.1249999553 3 8 242 0.2499998808 4 7 241 0.8124995828 5 6 -1 -0.00512894 -1 0.062915 -1 -0.0374451"
   "290 1.874998093 9 14 220 0.7187499404 10 13 293 0 11 12 -1 0.0166555 -1 0.000268223 -1 -0.0161227 221 2.208332539 15 16 -1 0.0788614 -1 -0.0330401 315 0.3950819671 18 19 -1 -0.0524417 196 -0.9999995232 20 25 45 1.499999642 21 24 260 0.05800369754 22 23 -1 -0.0285121 -1 0.0543147 -1 0.166264 246 1375000 26 27 -1 -0.0211607 -1 0.120831 -1 -0.0801771 29 0 0.7167333961 1 24 70 -0.4252688587 2 11 35 -0.9999995232 3 8 124 0.3928145468 4 5 -1 -0.0135705 4 0.5133105516 6 7 -1 0.157118 -1 -0.0315438 263 -1.031050444 9 10 -1 0.0106457 -1 -0.0426915 223 -0.4999999404 12 15 271 125000 13 14 -1 0.0512122 -1 -0.131705 16 -0.4285713732 16 23 304 0.1565067619 17 18 -1 0.00164978 310 78 19 22 306 0.08750978857 20 21 -1 0.0694819 -1 -0.0664709 -1 -0.0573051 -1 0.00129268 315 0.3992395401 25 26 -1 0.0495487 196 -0.9999995232 27 28 -1 -0.0308141 -1 0.0162932 29 247 0.1458333135 1 28 182 -0.9999998808 2 5 19"
   "1.999999642 3 4 -1 -0.0583792 -1 0.0551287 74 1.021190882 6 27 271 0.9444443583 7 22 125 5.424495697 8 21 102 -0.2999972105 9 10 -1 -0.0310348 307 20 11 16 271 0.06666664779 12 13 -1 0.0491115 280 -1.359484911 14 15 -1 0.0292505 -1 -0.0545583 265 -0.2083332688 17 20 306 0.003434647107 18 19 -1 -0.0688363 -1 0.0255829 -1 -0.0349883 -1 -0.0605829 298 -0.09999997914 23 26 159 -0.3999999166 24 25 -1 -0.036682 -1 0.0627913 -1 -0.0598709 -1 0.0586905 -1 -0.0502041 29 294 -0.3333332837 1 2 -1 -0.0422279 272 1.583332777 3 20 247 250000 4 19 270 1.374999285 5 14 17 -0.3478260338 6 11 14 -1.124999881 7 8 -1 -0.0863912 14 -0.3749999106 9 10 -1 0.0341524 -1 0.00251573 306 0.002587337047 12 13 -1 0.0797046 -1 -0.00639261 312 2.5 15 16 -1 -0.00999284 7 -0.3191046417 17 18 -1 0.0941918 -1 -0.0384051 -1 -0.0725471 226 1.999999046 21 22 -1 -0.0883052 316 300 23 28 6 0.5714253187 24 27 233 -0.5510421991 25"
   "26 -1 -0.0572068 -1 0.0811476 -1 -0.0799923 -1 -0.0669779 29 278 -0.009887663648 1 28 248 0.2749999464 2 21 185 -0.1426184922 3 8 118 -0.122864224 4 5 -1 0.00248971 303 0.06786871701 6 7 -1 0.00316258 -1 0.0710495 295 -0.1666666418 9 18 309 48 10 17 229 -0.01999999769 11 12 -1 -0.0564934 244 0.708333075 13 16 306 0.0426315628 14 15 -1 -0.00863045 -1 0.119247 -1 -0.0659036 -1 -0.00387377 102 -2.265290976 19 20 -1 0.0544535 -1 -0.0279396 244 -0.1249999702 22 23 -1 -0.0808217 271 0.6666665673 24 25 -1 -0.0276302 247 0.4583331943 26 27 -1 -0.0222591 -1 0.0198047 -1 0.00481635 29 277 -0.7751247287 1 28 248 0.3749998808 2 25 10 0.4242970347 3 24 235 0.6025767922 4 23 279 -0.8395937085 5 20 303 0.6158702374 6 17 240 -0.02444922365 7 10 303 0.01215680223 8 9 -1 0.0140416 -1 -0.0447766 305 0.02897423133 11 14 230 -1.006021023 12 13 -1 0.0308477 -1 -0.12151 188 -0.797652483 15 16 -1 -0.0537823 -1 0.0395676"
   "291 -2.593749285 18 19 -1 -0.0669304 -1 0.0229903 310 94 21 22 -1 -0.0284391 -1 0.0796162 -1 0.0703311 -1 0.0317763 221 1.041666269 26 27 -1 0.0515451 -1 -0.0462385 -1 -0.00195533 29 195 0.1666666418 1 28 122 0.001910449588 2 23 172 0.5714284778 3 22 22 -0.4999997616 4 15 314 10.5 5 12 294 -0.1666666418 6 9 266 0.4999999404 7 8 -1 0.0506282 -1 -0.0237941 11 -1.399999738 10 11 -1 0.0419725 -1 -0.0372987 291 -0.2187499404 13 14 -1 -0.0431473 -1 0.054207 128 0.1428571194 16 21 225 -0.4249999225 17 18 -1 -0.0379068 281 -0.812748313 19 20 -1 0.0381527 -1 -0.00622496 -1 -0.0398077 -1 -0.0584255 77 0.2334135771 24 25 -1 -0.0560021 6 -0.1249994561 26 27 -1 -0.033429 -1 0.0585377 -1 -0.0479001 29 114 1.166666508 1 20 303 0.1924695075 2 19 284 0.02137941122 3 4 -1 -0.0335207 240 -0.1957366467 5 14 284 0.09266272187 6 7 -1 0.0539962 306 0.1772293746 8 9 -1 -0.0225685 210 -0.6287042499 10 11 -1 -0.0231581"
   "295 0.1428571194 12 13 -1 0.218106 -1 -0.0290447 268 -0.0673076883 15 18 17 -0.5999999642 16 17 -1 -0.0503077 -1 0.0709877 -1 -0.0149862 -1 0.00648151 233 -0.6990063787 21 22 -1 -0.0320542 230 -0.2861846685 23 28 315 0.7112402916 24 27 314 6.5 25 26 -1 -0.00264426 -1 0.158685 -1 -0.0263994 -1 0.0187551 29 256 -0.6899793148 1 18 0 -0.7198616862 2 15 292 -0.4999994934 3 4 -1 -0.0739817 242 -0.1249999851 5 10 298 0.2499999404 6 9 248 -0.2499999106 7 8 -1 0.0561365 -1 -0.0215007 -1 0.0847122 313 0.5 11 14 306 0.1284901202 12 13 -1 -0.0605614 -1 0.139137 -1 -0.0209297 7 -2.204933167 16 17 -1 0.0578533 -1 -0.0525936 297 1.218749642 19 28 274 0.07142855972 20 27 273 0.3281249702 21 26 219 1.199999809 22 23 -1 -0.00107038 313 3.5 24 25 -1 -0.0383618 -1 0.18261 -1 0.0269819 -1 -0.00377727 -1 0.0292809 29 315 0.9938931465 1 28 130 -1.999999762 2 7 277 0.1135845184 3 6 306 0.6579782367 4 5 -1 0.0253653"
   "-1 -0.120618 -1 -0.0910609 271 0.7916663885 8 11 8 1.236990571 9 10 -1 0.00263332 -1 0.059336 305 0.3256161213 12 13 -1 0.0145525 0 -0.2446804494 14 15 -1 -0.0347105 247 0.4166665375 16 19 50 0.1894714832 17 18 -1 0.0320288 -1 -0.0262104 280 -0.9811421037 20 23 265 0.07142855972 21 22 -1 -0.359486 -1 -0.034449 215 0.7746725678 24 25 -1 -0.00368191 49 -0.1473795176 26 27 -1 -0.213406 -1 -0.0341817 -1 -0.0326445 29 149 0.1666666418 1 28 128 0.1428571194 2 27 58 1.799999595 3 26 50 0.003507261397 4 9 228 0.04166665301 5 8 250 -1.776356839e-15 6 7 -1 0.0138656 -1 0.0669617 -1 -0.0444869 263 0.1086626202 10 21 229 0.4374999404 11 18 6 -0.3243229091 12 17 20 -0.6249999404 13 14 -1 0.012726 54 1.466429114 15 16 -1 -0.0505129 -1 0.0351259 -1 0.0263094 266 0.5384614468 19 20 -1 0.0719748 -1 -0.0301177 50 0.1561657637 22 23 -1 -0.0190633 188 -0.4055403471 24 25 -1 -0.00818404 -1 0.059868 -1 0.059958"
   "-1 -0.0337292 -1 -0.037677 29 219 -0.3749999106 1 16 248 -0.02499999478 2 7 209 -0.7756491899 3 6 28 -0.07560540736 4 5 -1 0.130985 -1 0.0164878 -1 -0.0647823 250 0.2499999404 8 13 27 0.1904066354 9 10 -1 -0.0903887 225 -0.1249999404 11 12 -1 0.0667965 -1 -0.0669837 98 -0.2222209871 14 15 -1 0.0526788 -1 -0.0390832 14 -0.1666666418 17 20 259 -1.349993229 18 19 -1 -0.0672842 -1 0.0139091 129 -0.1666666418 21 22 -1 0.00738315 92 -1.089241147 23 24 -1 0.098484 13 -0.4999999106 25 26 -1 0.00330255 44 -3.666665554 27 28 -1 0.0796886 -1 -0.0395689 29 263 -22198.48047 1 4 215 1.10165906 2 3 -1 -0.0816204 -1 0.0554218 2 -4.08324337 5 12 223 0.9999995232 6 11 303 0.1082455143 7 8 -1 0.0516087 235 -1.653351068 9 10 -1 0.058419 -1 -0.0553055 -1 0.0754077 256 -1.263305306 13 18 248 -0.3749999106 14 17 248 -0.4249999225 15 16 -1 -0.0267593 -1 0.190549 -1 -0.053243 95 -0.2839443386 19 26 223 -0.3333332241"
   "20 21 -1 0.0582757 217 -0.2419282198 22 23 -1 -0.0588063 153 -0.2499999851 24 25 -1 0.102754 -1 -0.0227191 56 -0.118137002 27 28 -1 -0.00636789 -1 0.00532289 29 77 -0.1486852616 1 26 291 0.6666666269 2 25 186 0.4006657898 3 10 311 1.5 4 5 -1 0.00363198 185 0.03925921768 6 7 -1 -0.0117971 245 0.01041666605 8 9 -1 -0.0156312 -1 -0.0887253 49 -0.04066952318 11 14 37 -1.499999642 12 13 -1 0.071078 -1 -0.0185697 5 0.2879397571 15 18 100 -0.1708313525 16 17 -1 0.03308 -1 -0.0383536 213 0.01785702817 19 24 269 0.3749998212 20 21 -1 0.0344694 198 -0.2222221941 22 23 -1 0.0606853 -1 0.241532 -1 0.0266642 -1 -0.0432304 146 0.2611518502 27 28 -1 0.00697132 -1 -0.00605564 29 316 77 1 28 306 0.8340455294 2 27 16 0.7142856121 3 26 4 -0.9813854098 4 17 276 -1.617049813 5 6 -1 0.0212468 291 -0.1826922894 7 16 230 -0.8395404816 8 15 296 -8.881784197e-16 9 10 -1 -0.0383455 192 -0.02002933435 11 14 268 0.1770833135"
   "12 13 -1 -0.0708836 -1 0.0481454 -1 0.0514391 -1 0.0427497 -1 0.052964 275 0.02083333023 18 19 -1 0.0409081 260 0.009885141626 20 21 -1 -0.0599737 2 -0.3135558665 22 23 -1 0.0532244 286 0.4615426064 24 25 -1 -0.0587542 -1 0.0493215 -1 -0.0416046 -1 -0.0437621 -1 -0.0627018 29 245 -1.549999714 1 2 -1 -0.0505369 245 -1.333333135 3 4 -1 0.0689806 242 0.6874998212 5 26 239 0.5342226028 6 25 304 0.01431206148 7 8 -1 -0.0716266 227 0.2249999493 9 22 286 -0.5951952338 10 13 228 0.624999702 11 12 -1 0.012661 -1 0.119756 306 0.01687028818 14 17 67 -0.8571427464 15 16 -1 0.0586495 -1 -0.0715252 306 0.1128785238 18 21 278 0.1502342969 19 20 -1 -0.0216195 -1 0.0270966 -1 -0.0395103 217 -1.249976754 23 24 -1 0.0421684 -1 0.00394264 -1 -0.0380043 242 1.312499285 27 28 -1 0.0301215 -1 -0.0231473 29 255 1.889405131 1 28 245 0.5714284778 2 9 306 0.01259546727 3 8 255 -0.03617059439 4 5 -1 -0.0422221 260 -0.006864105817"
   "6 7 -1 0.074793 -1 -0.00891367 -1 0.00115621 243 0.04347825423 10 13 313 0.5 11 12 -1 0.0603368 -1 -0.0500287 50 -0.6692510247 14 17 163 0.0196560286 15 16 -1 0.325846 -1 0.0661778 274 0.4583332539 18 27 235 0.0482307449 19 22 199 0.6666664481 20 21 -1 -0.0656489 -1 0.0583059 232 0.2867373228 23 24 -1 0.190848 168 0.7261819243 25 26 -1 -0.00105533 -1 0.100928 -1 -0.021044 -1 -0.0622347 29 7 -2.535672665 1 6 180 -2.399999619 2 3 -1 0.0694144 217 0.3354812562 4 5 -1 -0.0288447 -1 0.0917494 2 -1.814297318 7 12 252 0.3928570747 8 11 249 6.499987125 9 10 -1 0.0306706 -1 0.13288 -1 -0.00886458 231 -2.420273066 13 14 -1 0.0752276 135 1.249999642 15 28 84 1.399999738 16 21 56 2.89974761 17 18 -1 0.00158951 6 1.749984741 19 20 -1 -0.0232728 -1 0.151616 196 -0.4999997616 22 27 243 0.2124999762 23 26 69 1.036538839 24 25 -1 0.0833976 -1 0.259441 -1 0.00216825 -1 0.00463009 -1 -0.0145581 29 315 0.9198217988"
   "1 4 275 -0.1428571194 2 3 -1 0.023253 -1 -0.0733622 274 -0.6718749404 5 6 -1 -0.0228952 220 -0.5249999166 7 12 251 -0.4687498808 8 11 307 7 9 10 -1 0.0566486 -1 -0.0109007 -1 0.0709335 266 -0.2749999464 13 14 -1 -0.0456134 13 -0.2857142389 15 20 121 -0.6666610837 16 19 273 -0.1666666418 17 18 -1 0.0527991 -1 -0.0575977 -1 0.0382885 254 0.4885778725 21 26 267 0.07499998808 22 23 -1 0.00610972 9 -1.400431752 24 25 -1 0.0265742 -1 -0.0704815 234 -1.564588904 27 28 -1 -0.0419024 -1 0.0501944 29 305 0.005248539615 1 12 225 -0.2499999702 2 7 312 0.5 3 4 -1 0.0588342 196 2.999996901 5 6 -1 -0.0655253 -1 0.125073 316 237 8 11 316 140 9 10 -1 -0.00688087 -1 0.0856065 -1 0.00619185 285 1.007636189 13 28 285 0.5776799321 14 23 239 0.5077194571 15 22 306 0.008340952918 16 21 208 -0.9226023555 17 20 7 -1.724374413 18 19 -1 -0.0823877 -1 0.107287 -1 -0.0972792 -1 0.000867975 -1 -0.0496925 242 0.6249998212"
   "24 25 -1 -4.44142e-05 304 0.4480936527 26 27 -1 0.0917217 -1 0.00217091 -1 -0.0711557 29 35 -0.4999998808 1 28 215 0.009582830593 2 3 -1 -0.0274521 191 0.4585247636 4 11 294 0.562499702 5 6 -1 -0.00281163 41 0.6666664481 7 8 -1 0.0145754 48 0.3196364343 9 10 -1 0.122227 -1 0.00412799 237 0.2105586231 12 25 260 -0.1023594961 13 14 -1 0.00929238 270 0.1499999762 15 16 -1 -0.156821 90 0.2499999404 17 24 44 -0.2499999404 18 19 -1 -0.0223946 39 -3.999995947 20 21 -1 0.0508394 100 0.1214047819 22 23 -1 -0.202662 -1 -0.00225243 -1 0.0197965 126 0.3749999404 26 27 -1 0.0489965 -1 -0.0471539 -1 0.00229558 29 134 -0.3749999404 1 20 124 -0.1504294425 2 7 87 -0.4999997616 3 4 -1 0.00654953 39 2.999996901 5 6 -1 -0.0182812 -1 -0.0808671 122 -0.3401061893 8 19 269 0.4615383148 9 18 295 0 10 13 266 -0.166666612 11 12 -1 -0.00123539 -1 -0.135929 307 95 14 17 1 0.2328147292 15 16 -1 -0.0167096 -1 -0.144884"
   "-1 0.0679393 -1 0.00844576 -1 0.0106363 82 0.3333332241 21 28 126 0.9999996424 22 23 -1 0.00951869 280 -0.577880919 24 25 -1 -0.0402443 61 -0.9999999404 26 27 -1 -0.118333 -1 0.00490326 -1 -0.00613354 29 115 0.01513232198 1 22 146 0 2 21 107 0.230769217 3 18 124 -0.3430064917 4 7 66 -0.1428571194 5 6 -1 0.0116831 -1 -0.0590019 287 -1.124999762 8 17 270 0.2499998808 9 14 287 -1.312499642 10 13 240 -36.6999321 11 12 -1 -0.0420175 -1 0.0559466 -1 -0.0475113 19 -2.249999523 15 16 -1 0.0486509 -1 -0.0739869 -1 0.0187589 132 -0.2499999404 19 20 -1 -0.0333055 -1 0.0582096 -1 -0.035169 2 -0.3135558665 23 28 28 0.2530245781 24 25 -1 -0.0337235 48 0.6310338974 26 27 -1 0.0577479 -1 -0.0580908 -1 -0.0683541 29 298 0.8124995828 1 12 290 1.032257915 2 7 167 0.2499981225 3 6 241 0.3124999106 4 5 -1 -0.00401803 -1 -0.039575 -1 0.00513273 76 0.086954467 8 11 263 0.1086626202 9 10 -1 -0.0253236 -1 0.102595"
   "-1 0.115822 244 0.7083331943 13 22 236 0.5212748051 14 21 219 0.1249999404 15 18 186 0.7783984542 16 17 -1 -0.0291362 -1 0.0990746 192 -0.07199151814 19 20 -1 0.0162544 -1 0.108341 -1 -0.030443 219 0.4249999225 23 28 264 0.06249999255 24 27 272 -0.8958332539 25 26 -1 0.058451 -1 -0.0918913 -1 0.0700895 -1 -0.0458758 29 15 -0.9999998808 1 16 217 -6899.641602 2 3 -1 -0.0610541 213 0.7857092619 4 13 19 1.666666389 5 10 228 0.5624998808 6 7 -1 0.00399634 307 16 8 9 -1 -0.0497316 -1 0.0738826 289 0.9861109853 11 12 -1 -0.122303 -1 0.0589619 293 -0.2499999404 14 15 -1 -0.0784911 -1 0.0748075 222 -0.8374999166 17 18 -1 -0.0532341 264 -0.3281249702 19 20 -1 -0.027277 213 1.187493086 21 28 94 0.2969889343 22 27 266 1.099999785 23 24 -1 0.00568333 245 -0.7708332539 25 26 -1 0.0894451 -1 -0.0387795 -1 -0.0189668 -1 -0.0518662 29 76 0.4084295332 1 28 94 0.08387231082 2 15 122 -0.8557007313 3 6 107 -0.3333332241"
   "4 5 -1 -0.00102729 -1 -0.0739517 3 0.01763442159 7 8 -1 -0.00289491 293 1.107142687 9 12 278 1.148601532 10 11 -1 0.0350497 -1 -0.099967 197 0.4444443882 13 14 -1 0.00924339 -1 -0.136166 311 0.5 16 21 122 -0.1824128628 17 20 140 -0.040587347 18 19 -1 -0.0716185 -1 0.0284304 -1 0.0237055 1 -0.1248280853 22 23 -1 0.00803019 72 -0.4293531179 24 27 304 0.003744066227 25 26 -1 -0.205832 -1 0.0332933 -1 0.0458738 -1 -0.00717742 29 195 0.1666666418 1 28 82 1.999999285 2 27 113 1.249999642 3 26 159 -0.9999998808 4 5 -1 -0.0401472 270 0.5312498808 6 23 74 1.021190882 7 22 310 124 8 13 248 -0.4999998808 9 10 -1 0.0414023 282 -1.229160547 11 12 -1 -0.0571163 -1 0.0227658 269 0.2499999106 14 19 192 -0.02648387849 15 18 204 -0.09999998659 16 17 -1 0.0343843 -1 -0.0460273 -1 0.0316634 10 -10.32578468 20 21 -1 0.0322455 -1 -0.0484943 -1 0.0703655 248 -0.1071428433 24 25 -1 0.0505828 -1 -0.00951306 -1 -0.046599"
   "-1 0.0581751 -1 -0.0441691 29 286 -15855.91113 1 2 -1 -0.0780433 298 0.8124995828 3 22 220 -2.343749523 4 5 -1 -0.0946153 21 -1.199999809 6 9 16 -0.3333332241 7 8 -1 0.0812767 -1 0.00820535 316 584 10 17 223 -1.776356839e-15 11 12 -1 -0.0203163 37 1.333332896 13 16 56 3.22218895 14 15 -1 -0.00375192 -1 0.0343376 -1 0.0378809 20 -0.7499998212 18 19 -1 0.0826366 195 2.399999619 20 21 -1 -0.0654971 -1 0.0566622 244 0.7708331943 23 26 236 0.5212748051 24 25 -1 0.0531031 -1 -0.0241894 219 -0.1249999851 27 28 -1 0.0783303 -1 -0.0315097 29 226 1250000 1 20 226 1.187499404 2 11 217 -12.49146557 3 6 315 0.719194293 4 5 -1 -0.0491122 -1 0.0104369 307 270 7 8 -1 0.0025449 246 875000 9 10 -1 -0.0519156 -1 0.0854171 18 -0.2499999404 12 13 -1 -0.0921212 239 -0.3237577081 14 19 2 0.4412440658 15 18 293 -0.09374997765 16 17 -1 0.0955301 -1 -0.0501815 -1 0.197827 -1 -0.0345124 260 0.245381996 21 28 307 158"
   "22 27 289 0.5499999523 23 26 211 0.1968913525 24 25 -1 -0.0217727 -1 0.098496 -1 -0.0351969 -1 0.102039 -1 -0.079437 29 197 -1.199999809 1 8 313 0.5 2 3 -1 -0.0549867 224 1.624998331 4 7 22 0.3333332241 5 6 -1 -0.0156481 -1 0.0259381 -1 -0.0740161 312 2.5 9 26 277 -0.5864046216 10 11 -1 0.0167861 1 -0.5919035673 12 13 -1 -0.0167731 316 44 14 15 -1 -0.03505 276 -0.4931841195 16 23 314 5.5 17 18 -1 0.028108 307 51 19 22 279 -0.186280027 20 21 -1 -0.139432 -1 0.0744379 -1 0.0314566 291 0.2187499404 24 25 -1 0.0265856 -1 -0.00147002 242 1625000 27 28 -1 -0.00532856 -1 0.13097 29 316 46 1 24 8 -1.813358903 2 3 -1 0.0367905 235 -1.521043301 4 9 291 -0.3333332837 5 8 11 -2.249999523 6 7 -1 0.0522942 -1 -0.0305505 -1 0.0303446 247 -0.3999999166 10 13 114 1.142856836 11 12 -1 0.0417638 -1 -0.0452841 78 0.3539324403 14 21 27 0.002046684036 15 20 2 -0.9143515229 16 19 310 125 17 18 -1 -0.0491655 -1"
   "0.0626376 -1 -0.0183202 -1 -0.033892 39 0.111111097 22 23 -1 -0.000443723 -1 0.0738207 286 -3.395215988 25 28 286 -10.1536808 26 27 -1 -0.0463306 -1 0.0522835 -1 -0.07032 29 288 -0.4999997616 1 6 312 2.5 2 3 -1 -0.0795203 45 0.1428571194 4 5 -1 -0.0557509 -1 0.0989627 294 -0.4374997914 7 8 -1 -0.0631841 290 -0.0624999851 9 22 288 0.8749991059 10 21 17 0 11 20 87 0.7499998212 12 19 80 -0.6666665673 13 14 -1 0.0880282 303 0.8807774782 15 16 -1 -0.00510649 295 -0.6458332539 17 18 -1 0.0939269 -1 -0.0326764 -1 0.108826 -1 -0.0432925 -1 -0.10267 280 -0.4523406923 23 26 213 1.499988794 24 25 -1 -0.0377414 -1 0.137009 298 -0.1249999553 27 28 -1 -0.0639687 -1 0.00349912 29 127 2.666665792 1 20 220 -1.124999762 2 3 -1 -0.0489896 213 1.499988794 4 17 77 0.505155921 5 10 69 0.378298223 6 9 95 0.1235584617 7 8 -1 0.00325449 -1 -0.0137707 -1 0.0151363 190 -0.9999930263 11 16 276 0.37556988 12 15 226 0.3499999344"
   "13 14 -1 0.0339414 -1 -0.0601734 -1 0.116314 -1 -0.0160098 7 -1.724374413 18 19 -1 0.0529099 -1 -0.0861045 246 1.916666031 21 28 282 -0.04166645929 22 25 175 -1.666666508 23 24 -1 -0.0820608 -1 0.0572467 23 0.4901400506 26 27 -1 -0.0628222 -1 0.0280731 -1 0.0752262 29 60 -3.749999046 1 10 91 -1.999999642 2 7 279 0.3552577198 3 6 130 -0.07142856717 4 5 -1 -0.0296994 -1 0.0866783 -1 -0.111997 284 0.3354203701 8 9 -1 -0.118558 -1 0.0228447 137 3.749999046 11 28 307 1 12 25 190 0.3333318532 13 24 211 0.265586853 14 23 108 -0.4285713732 15 18 266 -0.2499999702 16 17 -1 0.0680239 -1 -0.100735 240 -0.3276860416 19 22 285 0.4369337857 20 21 -1 -0.0439907 -1 0.102425 -1 0.0239165 -1 0.0360266 -1 -0.0522383 112 1.999997973 26 27 -1 0.00275863 -1 -0.0165984 -1 -0.0372242 29 247 0 1 28 125 2.643723249 2 27 292 -0.2499999553 3 8 289 -0.7142856121 4 7 287 -1.718749523 5 6 -1 0.0439446 -1 -0.0269267 -1"
   "0.0429479 8 -1.179260135 9 16 286 -4.692014217 10 13 316 25 11 12 -1 0.0522216 -1 -0.0222974 211 -1.259404063 14 15 -1 0.042378 -1 -0.0445622 76 -0.1575234979 17 18 -1 0.0526539 274 0.4999998212 19 26 21 -0.3333332241 20 23 281 -1.501022696 21 22 -1 -0.056267 -1 0.000722795 31 -0.060438402 24 25 -1 0.0193885 -1 -0.0749538 -1 0.028836 -1 -0.035101 -1 -0.0392422 29 151 0.4999999404 1 22 136 0 2 21 37 0.3333332241 3 10 241 0.2249999493 4 9 115 -0.6360406876 5 8 27 0.3318580091 6 7 -1 0.123236 -1 -0.0157992 -1 -0.00238667 -1 -0.0337455 179 0.6249999404 11 20 59 0.1428571194 12 19 98 0 13 18 116 0.04818243533 14 15 -1 0.00562174 217 -0.133823365 16 17 -1 0.0298968 -1 0.228448 -1 -0.024616 -1 0.0632643 -1 -0.0163594 -1 -0.0184141 232 -0.01855079085 23 26 76 -0.06942272931 24 25 -1 -0.0607456 -1 0.00365291 287 -0.04166666046 27 28 -1 -0.0242007 -1 0.0461383 29 272 -1.41666615 1 2 -1 -0.0744104 15"
   "-0.9999998808 3 14 196 -0.7999998331 4 9 269 6.999992847 5 6 -1 -0.00441102 296 0.7499997616 7 8 -1 -0.136737 -1 0.0335259 290 0.2499997467 10 13 228 0.5714284778 11 12 -1 0.0214589 -1 0.0859051 -1 -0.00110979 268 -0.2053571343 15 18 270 1.549999714 16 17 -1 -0.0266134 -1 0.0422457 164 0.6358777881 19 26 218 -0.1249999553 20 21 -1 -0.0113915 222 -0.07499999553 22 25 304 0.002716591349 23 24 -1 0.132216 -1 0.0190365 -1 0.00135111 266 -0.2678571045 27 28 -1 0.0545462 -1 -0.0389388 29 55 0.8978613019 1 26 46 0.2437672019 2 17 77 -0.1486852616 3 12 188 -0.9449350834 4 7 262 -0.1364931166 5 6 -1 0.038481 -1 -0.0616936 205 0.4999997616 8 9 -1 0.00332743 98 0.09090886265 10 11 -1 -0.00499121 -1 -0.0484293 191 -0.3215298653 13 14 -1 0.034335 15 0.4999998808 15 16 -1 0.00170946 -1 0.0393703 245 0.6022726893 18 23 77 -0.8044735789 19 20 -1 0.0589001 223 0.7499992251 21 22 -1 -0.0145995 -1 0.00493264"
   "55 0.3160486817 24 25 -1 -0.115861 -1 -0.0191987 278 0.8916866779 27 28 -1 0.00794216 -1 0.087701 29 122 0.04022618383 1 26 58 1.799999595 2 25 315 0.9198217988 3 4 -1 -0.049876 306 0.05012866482 5 20 230 -0.8522069454 6 19 281 -0.8004301786 7 18 22 -0.4999997616 8 13 238 -1.269052029 9 10 -1 0.014929 293 -0.3333332241 11 12 -1 -0.00642806 -1 -0.0788971 286 -3.919728279 14 15 -1 0.0522547 264 -0.03124999255 16 17 -1 -0.0591747 -1 0.0307234 -1 -0.0495095 -1 0.0470458 316 4 21 24 228 0.1749999672 22 23 -1 0.0511309 -1 -0.0465282 -1 -0.0267883 -1 0.0489832 213 0.1874990612 27 28 -1 -0.0628725 -1 0.0147171 29 245 -1.549999714 1 2 -1 -0.049412 80 0.5999999642 3 18 98 -0.2307686061 4 15 6 -0.1428563297 5 12 127 0.2499999702 6 11 53 -0.04184124246 7 8 -1 -0.0774543 14 -0.6666664481 9 10 -1 -0.0553584 -1 0.0396745 -1 0.0659499 247 -0.312499851 13 14 -1 0.083721 -1 -0.0188995 58 0.6666665673 16 17"
   "-1 -0.0141041 -1 0.0110647 46 0.5121764541 19 20 -1 -0.000552379 61 1.090909004 21 26 217 0.4068328142 22 25 270 -0.06249998882 23 24 -1 -0.00404522 -1 0.070687 -1 -0.117054 277 0.3105548918 27 28 -1 -0.0754908 -1 0.0309792 29 44 -0.9999998808 1 16 126 -0.9999998212 2 3 -1 0.0214417 31 -0.6469875574 4 7 133 1.333333135 5 6 -1 -0.0795762 -1 0.0327883 134 0.3333332837 8 13 147 -0.7023836374 9 10 -1 0.0198628 290 -0.5999999046 11 12 -1 0.0999835 -1 -0.0436965 188 0.3967905939 14 15 -1 0.0221144 -1 -0.0679145 192 0.684604764 17 18 -1 -0.000361799 3 -0.03121471591 19 20 -1 0.00367865 63 -0.2499999404 21 22 -1 -0.0148629 208 0.3955827355 23 28 293 0.562499702 24 27 255 0.1626493037 25 26 -1 -0.041614 -1 0.108838 -1 0.173519 -1 0.0121981 29 130 -0.3333333135 1 18 269 0.624999404 2 17 244 -0.2142856866 3 4 -1 0.0252804 248 0.0624999702 5 10 89 -0.6999999285 6 7 -1 0.0220699 149 0.4999999106 8 9 -1"
   "-0.0965548 -1 0.00567099 8 0.3679898083 11 12 -1 0.00196127 286 -0.6632973552 13 14 -1 0.00835951 54 -0.6120584011 15 16 -1 0.00249944 -1 -0.0826716 -1 0.006505 311 2.5 19 22 0 -0.0291968789 20 21 -1 0.000907705 -1 0.018179 61 -0.2999999821 23 26 99 -0.4805891514 24 25 -1 -0.152046 -1 -0.0286544 95 0.3106531501 27 28 -1 -0.00644465 -1 0.0475445 29 294 0 1 28 54 -0.279746592 2 3 -1 -0.0328759 244 -0.9249998331 4 7 44 0 5 6 -1 0.00304827 -1 0.0767427 36 -0.7499998212 8 9 -1 0.0499901 197 -0.1777777672 10 11 -1 -0.0275793 208 -0.602679491 12 23 27 0.0008733270806 13 14 -1 0.0145901 65 1.666666389 15 22 270 0.04166665301 16 19 238 -1.220059037 17 18 -1 0.02997 -1 -0.0343431 285 -1.651744127 20 21 -1 0.0266799 -1 -0.0612619 -1 0.0397618 193 -0.5402074456 24 25 -1 -0.0300199 270 -0.03124999255 26 27 -1 -0.0135546 -1 0.0489495 -1 -0.0453733 29 286 -15855.91113 1 2 -1 -0.076439 231 1.097498536 3"
   "28 239 0.7885842919 4 27 257 0.8692324162 5 24 312 2.5 6 7 -1 -0.0106094 278 -0.2226434648 8 13 311 2.5 9 10 -1 0.0195179 89 0.3999999166 11 12 -1 0.0416845 -1 0.180947 204 1.124999881 14 21 306 0.01687028818 15 18 147 -0.2603159249 16 17 -1 0.0200147 -1 -0.0612497 218 0.2749999464 19 20 -1 0.0143894 -1 -0.029266 293 0.3928570747 22 23 -1 -0.0265555 -1 0.0548781 232 0.6441478133 25 26 -1 0.0317673 -1 -0.0844054 -1 -0.0336235 -1 0.0560085 29 304 0.8536652923 1 28 26 0.2262078822 2 23 311 1.5 3 8 304 0.008792823181 4 7 192 1.048442721 5 6 -1 -0.0466094 -1 0.135216 -1 -0.000855632 26 0.03571890667 9 14 187 -0.0006125607179 10 13 189 -0.6991145015 11 12 -1 0.0531529 -1 -0.026885 -1 0.0155888 145 -0.8504810929 15 16 -1 -0.0514795 122 -0.3300733566 17 22 48 -0.07228645682 18 19 -1 -0.0244212 25 0.4082280397 20 21 -1 0.129811 -1 -0.00989814 -1 0.0229248 24 0.1350918263 24 25 -1 -0.0412391 227 0"
   "26 27 -1 0.0137467 -1 -0.00682787 -1 -0.0671786 29 32 0.5264111757 1 20 31 0.214352265 2 15 42 0.4999999106 3 14 78 0.01077239215 4 13 71 -0.1664408594 5 12 72 0.1676082164 6 11 263 -1.132259488 7 8 -1 0.0166464 29 -1.133329153 9 10 -1 0.0489869 -1 -0.0512859 -1 0.0483275 -1 0.00196356 -1 0.015047 -1 -0.0117411 289 0.4249999225 16 19 146 0.18073906 17 18 -1 -0.00436411 -1 -0.040197 -1 0.0122446 271 0.7916663885 21 22 -1 0.0150751 305 0.3256161213 23 24 -1 0.0282264 283 0.3021407425 25 26 -1 -0.0450206 124 0.4814270437 27 28 -1 -0.0237737 -1 0.0907729 29 115 0.01513232198 1 22 69 1.445707202 2 21 27 0.98634547 3 16 307 20 4 9 280 -1.359484911 5 6 -1 0.0383079 4 -0.9813854098 7 8 -1 -0.0267909 -1 0.0352439 307 31 10 11 -1 -0.0400192 46 0.0550798662 12 15 219 -0.2187499404 13 14 -1 0.00728942 -1 -0.0553574 -1 0.0415326 80 -0.5999999046 17 18 -1 0.0532196 105 0.1666666418 19 20 -1 -0.000648793"
   "-1 -0.0799127 -1 0.053757 2 -0.3135558665 23 28 254 -0.2881116867 24 27 260 -0.8260483146 25 26 -1 0.0133185 -1 -0.0659173 -1 0.0258516 -1 -0.0659002 29 43 -1.749999523 1 8 274 0.02083333023 2 3 -1 -0.0223498 227 0.7499992251 4 7 251 0.6249998212 5 6 -1 0.0968871 -1 -0.0466185 -1 -0.0296951 271 125000 9 28 73 -0.7087602019 10 13 109 -0.1428571194 11 12 -1 -0.0542723 -1 0.0569836 213 -1.339279056 14 19 4 -0.5118960738 15 18 285 0.7830975056 16 17 -1 -0.0748546 -1 0.0923714 -1 0.0280617 228 -0.5312498808 20 23 223 0.1249998733 21 22 -1 0.0424754 -1 -0.0389429 242 0.7812498212 24 25 -1 -0.00718122 242 1.374998569 26 27 -1 0.0371043 -1 -0.0255974 -1 -0.0580931 29 249 2500000 1 26 226 1.104166508 2 11 271 1750000 3 10 82 -1.499999762 4 5 -1 -0.0250771 180 0.4999999404 6 9 11 0.3333332241 7 8 -1 -0.00796221 -1 0.00856306 -1 0.0111932 -1 -0.0573778 18 -0.2499999404 12 13 -1 -0.0850866 310 158 14"
   "19 130 -0.3076922894 15 16 -1 0.0395788 122 0.9051359296 17 18 -1 -0.0551969 -1 0.0395844 303 0.001009741216 20 21 -1 0.149811 207 0.3879427612 22 25 314 7.5 23 24 -1 -0.0344682 -1 0.0694251 -1 -0.103943 314 8.5 27 28 -1 -0.0291538 -1 0.0742402 29 315 0.9877551198 1 24 315 0.9279835224 2 19 316 53 3 16 316 32 4 5 -1 0.0642561 251 -0.2812499404 6 7 -1 0.0520124 274 0.04166666046 8 11 214 -0.04444437474 9 10 -1 0.0105222 -1 -0.14948 220 -0.09374997765 12 15 227 0.2916665673 13 14 -1 0.0195197 -1 -0.251795 -1 -0.0186572 304 0.001817294513 17 18 -1 0.0538462 -1 -4.50945e-05 252 -0.1666666418 20 23 126 1.749999762 21 22 -1 -0.00223472 -1 -0.102808 -1 0.0474336 161 -0.5887116194 25 26 -1 0.0475907 253 -1.043060064 27 28 -1 0.0209768 -1 -0.0556459 29 149 0.1666666418 1 28 128 0.1428571194 2 27 66 -1.166666508 3 4 -1 -0.0494732 12 0.7142856121 5 26 159 -0.7499998808 6 7 -1 -0.0415974 210 -0.1496752501"
   "8 25 50 0.005054386798 9 16 258 -1.043045402 10 13 310 123 11 12 -1 -0.0660078 -1 0.021918 243 -0.5416665673 14 15 -1 0.069923 -1 0.00104663 307 4 17 24 267 0.07499998808 18 21 125 -5.744268417 19 20 -1 -0.0271667 -1 0.0651609 9 -1.335283279 22 23 -1 0.049764 -1 -0.0466998 -1 -0.021477 -1 0.0372319 -1 0.0624037 -1 -0.0310774 -1 -0.0340251 29 34 0.4999999106 1 8 149 -0.8333331943 2 5 89 0.9999996424 3 4 -1 0.00479851 -1 0.0647396 149 0.3333332241 6 7 -1 -0.0173776 -1 0.00541653 101 -0.686996758 9 10 -1 0.0619454 305 0.003275237745 11 12 -1 0.0601153 272 0.5312498808 13 26 312 1.5 14 15 -1 -0.0221073 303 0.230530113 16 25 272 0.07692306489 17 18 -1 -0.0218016 201 0.1428571194 19 22 105 0.3999999166 20 21 -1 -0.0649954 -1 0.0606643 272 0.2916665673 23 24 -1 0.109407 -1 0.008346 -1 0.0622478 261 0.3740905821 27 28 -1 -0.0566526 -1 0.0273495 29 256 0.6352572441 1 12 256 0.1383901983 2 3 -1 -0.00309425"
   "316 366 4 9 309 89 5 6 -1 0.0390949 241 -0.2749999464 7 8 -1 -0.0292999 -1 0.0153005 291 0.5937499404 10 11 -1 -0.0197171 -1 0.0954175 309 173 13 20 283 -0.01308128703 14 15 -1 0.00290436 283 0.5843098164 16 19 304 0.0402447246 17 18 -1 -0.0484899 -1 -0.1748 -1 -0.0327095 289 0.2291666269 21 26 212 0.8125032187 22 23 -1 -0.0459965 248 0.3333332241 24 25 -1 0.163865 -1 -0.0725199 218 -0.03124999255 27 28 -1 0.0848612 -1 0.0085555 29 197 -0.9999998808 1 10 22 0.4285713732 2 5 307 3 3 4 -1 -0.0717065 -1 -0.0166796 51 -0.08945078403 6 9 114 -1.714285493 7 8 -1 -0.0337001 -1 0.0654136 -1 -0.0108114 312 2.5 11 28 277 -0.5864046216 12 13 -1 0.0166152 3 -0.5149277449 14 21 310 40 15 18 257 -0.1208411381 16 17 -1 -0.026274 -1 0.0891914 221 0.1666666418 19 20 -1 0.00145729 -1 -0.0321728 316 44 22 23 -1 -0.0328971 291 0.2187499404 24 27 277 -0.3309913576 25 26 -1 -0.0210144 -1 0.0330462 -1 -0.00182721"
   "-1 -0.00409989 29 297 -0.1666666418 1 28 265 0.4583332539 2 23 305 0.001603788231 3 4 -1 0.0503598 271 1.874998093 5 22 58 1.799999595 6 21 263 0.1086626202 7 16 290 -0.6249999404 8 11 46 -0.003779435297 9 10 -1 -0.0582512 -1 0.032107 268 0.6339285374 12 15 265 -0.2499998808 13 14 -1 -0.00773606 -1 -0.0531377 -1 0.0375896 0 -1.222980142 17 18 -1 0.0331721 288 -0.3749999702 19 20 -1 -0.0424116 -1 0.0297383 -1 0.0612168 -1 0.0554602 313 0.5 24 25 -1 -0.0336392 2 -0.05207502842 26 27 -1 0.0668755 -1 -0.0483571 -1 -0.0210385 29 288 -0.4999997616 1 6 41 0.1428571194 2 3 -1 -0.0706215 312 2.5 4 5 -1 -0.0577674 -1 0.0854007 310 123 7 26 246 -0.02083333023 8 9 -1 -0.0567938 213 0.2187489122 10 21 315 0.2317327708 11 12 -1 0.0643173 262 0.4301501811 13 16 8 0.06892836094 14 15 -1 0.000254712 -1 -0.0664673 256 -0.6536671519 17 18 -1 -0.0855921 251 0.5937498808 19 20 -1 0.067399 -1 -0.0276749 223 1.624998331"
   "22 25 79 -0.3749959171 23 24 -1 0.0040873 -1 -0.0563161 -1 0.0234932 316 504 27 28 -1 0.0114508 -1 -0.0235822 29 248 -0.9999989867 1 4 293 -0.2708332837 2 3 -1 -0.0291003 -1 0.104483 225 -0.02499999478 5 28 263 0.2530050874 6 21 15 -0.9999998212 7 12 306 0.03739068657 8 9 -1 -0.0477398 21 -0.4999998808 10 11 -1 -0.0438988 -1 0.0523971 266 -0.909090817 13 16 211 0.246297732 14 15 -1 -0.0604063 -1 0.179248 256 0.01750070229 17 18 -1 -0.0552375 221 0.7592591643 19 20 -1 -0.000210044 -1 -0.126398 32 -0.3943057358 22 23 -1 -0.0546683 291 -0.7874999046 24 25 -1 -0.0455632 30 -0.007428738754 26 27 -1 -0.0347872 -1 0.0529111 -1 0.00157413 29 62 -5.999997139 1 12 181 0.4285713732 2 11 14 0.9999997616 3 8 84 1.076923013 4 7 63 0.7499998212 5 6 -1 -0.0485937 -1 0.0186096 -1 0.0591969 265 0.4374998808 9 10 -1 0.0992198 -1 -0.023489 -1 -0.0638844 192 -0.4149622023 13 16 312 0.5 14 15 -1 -0.0290882 -1"
   "0.000330444 184 0.1283666193 17 18 -1 0.00972339 240 -0.1304151416 19 28 16 0.1999999583 20 27 315 0.183116883 21 22 -1 -0.0389006 291 0.1805555373 23 26 228 0.4749999046 24 25 -1 0.0021826 -1 -0.0380105 -1 0.0195819 -1 0.0216776 -1 -0.0174338 29 123 0.3797419965 1 28 195 0.1666666418 2 27 82 1.999999285 3 26 113 1.249999642 4 25 44 0.6666665673 5 22 119 -0.3115239739 6 9 29 0.2727260292 7 8 -1 0.0169544 -1 -0.058257 78 0.2596122622 10 19 188 -0.0001208916729 11 14 52 0.5714265108 12 13 -1 -0.0398449 -1 0.0191624 32 0.6193015575 15 18 220 0.1718749851 16 17 -1 0.0217377 -1 -0.0435047 -1 -0.0613854 306 0.002806749661 20 21 -1 -0.0481474 -1 0.051449 314 11.5 23 24 -1 0.0513883 -1 -0.0148094 -1 -0.0411616 -1 0.0554514 -1 -0.0403963 -1 -0.0464811 29 272 1.583332777 1 22 228 0.8906248808 2 15 292 0.9749997854 3 6 310 123 4 5 -1 -0.00953549 -1 0.012174 32 -0.8497903943 7 8 -1 0.0423207 9 -1.335283279"
   "9 10 -1 0.048234 247 1.54166615 11 14 57 0.5714284778 12 13 -1 -0.0461708 -1 -0.00297834 -1 0.0404056 287 0.3249999285 16 21 263 -4.440892099e-16 17 20 7 -0.7129709125 18 19 -1 -0.0273386 -1 0.0611709 -1 0.110125 -1 -0.0340935 226 1.999999046 23 24 -1 -0.082227 316 300 25 28 8 -0.6729235053 26 27 -1 0.0818586 -1 -0.0160458 -1 -0.0590107 29 240 0.9012978673 1 28 315 0.9802469015 2 25 315 0.9279835224 3 22 316 53 4 19 316 37 5 6 -1 -0.0319022 268 0.4305555224 7 16 251 -0.4687498808 8 9 -1 -0.082999 220 0.1249999404 10 13 238 0.6634836197 11 12 -1 0.16923 -1 -0.0645357 260 0.1902559847 14 15 -1 0.115226 -1 -0.00211071 289 0.2249999493 17 18 -1 -0.084309 -1 0.0489218 281 -0.5091727376 20 21 -1 0.0115633 -1 -0.00323852 252 -0.1666666418 23 24 -1 0.00591104 -1 -0.0530131 266 0.1749999672 26 27 -1 -0.0125028 -1 0.0541527 -1 -0.0769821 29 60 -3.749999046 1 6 91 -1.799999595 2 3 -1 -0.000796678 99"
   "-0.1998452246 4 5 -1 0.0545417 -1 -0.106333 240 0.589022398 7 20 294 -0.1249999553 8 19 303 0.01215680223 9 10 -1 0.0217213 248 -0.1874999106 11 14 252 0.2499998808 12 13 -1 -0.00901934 -1 0.0537549 215 -0.3536948562 15 16 -1 0.0114369 262 0.5090000629 17 18 -1 -0.0707114 -1 0.000910694 -1 0.0016131 296 -0.3749998808 21 24 218 0.9249998331 22 23 -1 -0.000466102 -1 -0.170393 248 0.624999404 25 28 291 0.7166666389 26 27 -1 0.0734441 -1 -0.0753745 -1 -0.0651133 29 247 0.1458333135 1 28 274 -0.6718749404 2 3 -1 -0.021904 220 -0.5249999166 4 5 -1 0.0291606 266 -0.2678571045 6 9 0 -1.222980142 7 8 -1 0.00444209 -1 -0.0677734 246 -0.08333330601 10 21 142 -0.6906071901 11 12 -1 -0.0180702 272 0.5833331347 13 20 17 -0.1052631363 14 17 7 -0.7374042869 15 16 -1 -0.0114812 -1 0.0713165 7 -0.3049671054 18 19 -1 0.0318982 -1 -0.0548841 -1 -0.0200316 243 -0.3124999702 22 27 159 -0.8571427464 23 24 -1 -0.0590923"
   "25 0.01652575843 25 26 -1 -0.0234297 -1 0.035279 -1 -0.0353592 -1 -0.0440255 29 217 0.479398191 1 22 10 0.6274150014 2 7 218 0.7343748808 3 4 -1 0.00122523 305 0.007610102184 5 6 -1 0.0314461 -1 -0.0457664 265 0.6718749404 8 21 12 -0.4999999106 9 12 169 1.184675097 10 11 -1 0.0186763 -1 -0.101752 315 0.1098265871 13 14 -1 -0.0867571 132 -0.1666666418 15 16 -1 -0.0143581 2 -0.5995899439 17 20 164 0.6688891649 18 19 -1 -0.0444182 -1 0.0913094 -1 0.118578 -1 -0.0529904 260 -0.1771725118 23 28 274 -0.1999999583 24 25 -1 -0.0665482 264 0.3333332241 26 27 -1 -0.0176803 -1 0.111369 -1 -0.0910907 29 253 -1.396643996 1 2 -1 -0.0383294 315 0.9802469015 3 24 315 0.9279835224 4 17 316 53 5 16 235 0.9360509515 6 15 5 0.2879397571 7 10 265 -0.3749998212 8 9 -1 0.199978 -1 -0.00223505 310 89 11 12 -1 0.0477986 228 0.3249999285 13 14 -1 0.220287 -1 -0.0296363 -1 -0.127519 -1 -5.63487e-05 298 -0.1458333135"
   "18 23 312 0.5 19 20 -1 0.0425156 304 0.02367858589 21 22 -1 0.0133518 -1 -0.0595741 -1 -0.0607866 7 0.157028541 25 28 266 0.1749999672 26 27 -1 0.00120133 -1 0.0670321 -1 -0.0678689 29 99 0.8357346654 1 14 99 0.09283752739 2 9 11 0.3333332241 3 8 115 -0.2490411401 4 5 -1 -0.0135168 130 -0.9999998808 6 7 -1 -0.0456488 -1 0.0078786 -1 -0.0109677 122 0.01949405856 10 13 224 2.562498808 11 12 -1 -0.00494709 -1 0.114336 -1 0.0238599 117 0.2799765468 15 28 237 0.1425720602 16 23 35 -1.124999881 17 20 265 0.2499998808 18 19 -1 0.108834 -1 0.00085283 37 1.499999285 21 22 -1 -0.0193898 -1 0.0831693 69 0.07584322244 24 27 235 0.4148980677 25 26 -1 0.0318421 -1 -0.0955384 -1 -0.0917332 -1 0.0391583 29 149 0.2857142389 1 28 133 -0.2499999404 2 5 42 0.3999999166 3 4 -1 -0.0554516 -1 0.0199599 22 -0.4999997616 6 15 290 -0.9999997616 7 8 -1 0.0347705 316 23 9 14 304 0.00424536597 10 13 259 0.3999980092"
   "11 12 -1 0.0331521 -1 -0.0463055 -1 -0.027622 -1 -0.0563654 9 -1.053793311 16 19 43 -0.2499999404 17 18 -1 -0.0469465 -1 0.0443071 250 -0.3333332837 20 21 -1 0.0237954 238 -1.18833518 22 23 -1 -0.0605268 237 -0.3795868456 24 27 67 0.6666665673 25 26 -1 0.0405834 -1 -0.0445446 -1 -0.0475957 -1 -0.0373391 29 105 -1.199999809 1 16 51 -0.1505404264 2 7 178 1.66666615 3 6 9 -0.8529111743 4 5 -1 0.114249 -1 -0.0552333 -1 0.0859776 226 -0.7321427464 8 9 -1 -0.0714099 241 0.4749999046 10 15 191 0 11 12 -1 -0.0107122 246 0.7499992251 13 14 -1 0.140688 -1 0.0254741 -1 -0.0079365 293 125000 17 28 178 -0.3999998271 18 23 213 1.187493086 19 20 -1 -0.0336336 36 0.2857142389 21 22 -1 -0.0379858 -1 0.119771 145 -1.10672009 24 25 -1 0.0663461 199 -0.1249999851 26 27 -1 -0.0374866 -1 0.00232107 -1 -0.0640126 29 161 -1.427970648 1 4 199 -0.1428571343 2 3 -1 -0.046084 -1 0.010334 130 -0.3636363447 5 22 107 -0.8181817532"
   "6 7 -1 -0.0182634 53 0.06921435148 8 19 304 0.05516370013 9 14 93 0.02337423339 10 13 48 0.06286104769 11 12 -1 -0.00289616 -1 0.0661741 -1 -0.00649278 26 0.1218636259 15 16 -1 0.095766 290 0.0624999851 17 18 -1 -0.0225798 -1 0.0569505 116 0.06769489497 20 21 -1 0.01055 -1 -0.0575838 315 0.9802469015 23 26 229 -0.009615384042 24 25 -1 -0.0148359 -1 0.000419731 253 -1.004416466 27 28 -1 -0.0512836 -1 0.0408213 29 284 1.029084802 1 24 5 1.119276643 2 17 285 0.3958349824 3 8 266 -1.395833135 4 5 -1 0.0462761 276 0.2233695388 6 7 -1 0.000149911 -1 -0.0131637 267 0.4999994934 9 10 -1 0.0184976 7 -0.3191046417 11 14 270 0.312499851 12 13 -1 0.00669777 -1 -0.0411671 215 0.8202653527 15 16 -1 0.026802 -1 -0.0678702 214 0.05015873164 18 19 -1 0.0512953 36 0.1818181723 20 23 253 0.3408627808 21 22 -1 -0.0697779 -1 0.0780508 -1 -0.0947414 250 0.3437499106 25 26 -1 0.0416477 279 -0.3240794539 27 28 -1"
   "0.0645828 -1 -0.0617751 29 244 0.01562499814 1 26 100 -0.1708313525 2 3 -1 -0.0284571 22 -0.4999997616 4 19 283 -0.7070052624 5 12 14 -0.4999999106 6 9 0 -1.247945547 7 8 -1 0.0227654 -1 -0.0392197 11 -1.499999642 10 11 -1 0.0225189 -1 -0.0731406 304 0.004176810849 13 16 13 -0.2857142389 14 15 -1 0.0656449 -1 -0.0387142 254 -0.1896498352 17 18 -1 -0.053354 -1 0.026175 281 -0.826338172 20 21 -1 0.0237152 241 -0.8124999404 22 23 -1 -0.0665366 315 0.9407407641 24 25 -1 -0.0707523 -1 0.0348192 314 2.5 27 28 -1 0.0306782 -1 -0.0569945 29 42 -1.499999642 1 14 6 0.5555524826 2 13 259 -0.4499968588 3 4 -1 -0.0313 273 1.374998569 5 12 0 -0.0360891521 6 11 276 0.09619604051 7 8 -1 0.00392752 264 0.9843748808 9 10 -1 0.121118 -1 0.00273899 -1 -0.0197311 -1 -0.0655317 -1 -0.0345765 200 1.749999523 15 22 14 -0.2499999702 16 21 213 -1.339279056 17 18 -1 -0.0426902 262 -0.3148043454 19 20 -1 0.036181 -1"
   "-3.13946e-05 -1 -0.0122344 228 0.624999702 23 28 116 0.2363311648 24 25 -1 -0.0139708 7 -0.3191046417 26 27 -1 0.0539218 -1 -0.0769669 -1 0.0743916"]))

(def qlc-ml-fine-classes-used {"PRE" 0.5 "DIP" 0.7 "OUT" 0.6})   ; taerskler fra testen (r526)

(defn qlc-ml-parse
  "traeer som tekst (formatet i qlc-ml-model-text) -> {:classes :base :trees}, eller nil"
  [^String text]
  (try
    (let [tok  (java.util.Scanner. text)
          nxt  (fn [] (.next tok))
          K    (Long/parseLong (nxt))
          cls  (vec (repeatedly K nxt))
          n    (Long/parseLong (nxt))
          base (double-array (repeatedly K #(Double/parseDouble (nxt))))
          tree (fn []
                 (let [c  (Long/parseLong (nxt))
                       fe (int-array c) th (double-array c) le (int-array c) ri (int-array c)]
                   (dotimes [i c]
                     (let [f (Long/parseLong (nxt))]
                       (aset fe i (int f))
                       (aset th i (Double/parseDouble (nxt)))
                       (when-not (neg? f)
                         (aset le i (int (Long/parseLong (nxt))))
                         (aset ri i (int (Long/parseLong (nxt)))))))
                   [fe th le ri]))
          trees (vec (repeatedly n (fn [] (vec (repeatedly K tree)))))]
      {:classes cls :base base :trees trees})
    (catch Throwable e
      (try (timbre/warn "QLC+ AI-modellen kunne ikke laeses:" (.getMessage e)) (catch Throwable _ nil))
      nil)))

(def qlc-ml-fine-model (delay (qlc-ml-parse qlc-ml-fine-text)))

(def qlc-ml-states-order ["BREAK" "BUILD" "DROP" "GROOVE"])

(defn qlc-ml-fine-features
  "pr. beat: beatet z-scoret i nummeret, dets naboer (+-4 beats), straekningerne omkring (+-16), sektionen
   modellen staar i (og dens sandsynligheder), hvor laenge den har staaet / staar endnu, afstanden til
   naeste og forrige drop, og hvor i takten/nummeret - samme regning som traeningen (train_fine.py)"
  [rows starts states probs]
  (let [n    (count rows)
        F    (alength ^doubles (first rows))
        stat (mapv (fn [f] (let [a (double-array (map #(aget ^doubles % f) rows))]
                             (java.util.Arrays/sort a)
                             [(qlc-ml-pct a 50) (+ (- (qlc-ml-pct a 75) (qlc-ml-pct a 25)) 1e-6)]))
                   (range F))
        Z    (object-array (map (fn [^doubles r] (let [z (double-array F)]
                                                   (dotimes [f F] (let [[m q] (nth stat f)] (aset z f (/ (- (aget r f) m) q))))
                                                   z))
                                rows))
        cs   (let [c (object-array (inc n))]
               (aset c 0 (double-array F))
               (dotimes [i n]
                 (let [p ^doubles (aget c i) z ^doubles (aget Z i) s (double-array F)]
                   (dotimes [f F] (aset s f (+ (aget p f) (aget z f))))
                   (aset c (inc i) s)))
               c)
        cl   (fn [x] (max 0 (min n x)))
        win  (fn [i a b]
               (let [lo (cl (+ i a)) hi (cl (+ i b))]
                 (if (<= hi lo)
                   (seq ^doubles (aget Z i))
                   (let [h ^doubles (aget cs hi) l ^doubles (aget cs lo) c (double (- hi lo))]
                     (map (fn [f] (/ (- (aget h f) (aget l f)) c)) (range F))))))
        bar-of (let [b (int-array n)]
                 (doseq [[j s] (map-indexed vector starts)]
                   (loop [x (long s)] (when (< x n) (aset b x (int j)) (recur (inc x)))))
                 b)
        pb   (mapv #(nth states (aget bar-of %)) (range n))
        drop-start? (fn [i] (and (= "DROP" (nth pb i)) (or (zero? i) (not= "DROP" (nth pb (dec i))))))
        since (let [a (double-array n)]
                (loop [i 0 s 0] (when (< i n)
                                  (let [s (if (and (pos? i) (not= (nth pb i) (nth pb (dec i)))) 0 s)]
                                    (aset a i (double s)) (recur (inc i) (inc s)))))
                a)
        to   (let [a (double-array n)]
               (loop [i (dec n) t 0] (when (>= i 0)
                                       (let [t (inc (if (and (< i (dec n)) (not= (nth pb i) (nth pb (inc i)))) 0 t))]
                                         (aset a i (double t)) (recur (dec i) t))))
               a)
        tod  (let [a (double-array n)]
               (loop [i (dec n) nx 999.0] (when (>= i 0)
                                            (let [nx (if (drop-start? i) 0.0 nx)]
                                              (aset a i (min nx 256.0)) (recur (dec i) (+ nx 1.0)))))
               a)
        sd   (let [a (double-array n)]
               (loop [i 0 l 999.0] (when (< i n)
                                     (let [l (if (drop-start? i) 0.0 l)]
                                       (aset a i (min l 256.0)) (recur (inc i) (+ l 1.0)))))
               a)
        fl   (fn [x] (double (float x)))]
    (mapv (fn [i]
            (let [z ^doubles (aget Z i)
                  j (aget bar-of i)
                  st (nth pb i)]
              (double-array
                (map fl
                     (concat (seq z)
                             (mapcat (fn [o] (let [zo ^doubles (aget Z (max 0 (min (dec n) (+ i o))))]
                                               (map (fn [f] (- (aget zo f) (aget z f))) (range F))))
                                     [-4 -3 -2 -1 1 2 3 4])
                             (mapcat (fn [[a b]] (win i a b)) [[-8 0] [1 9] [-16 -8] [9 17]])
                             (map #(if (= st %) 1.0 0.0) qlc-ml-states-order)
                             (nth probs j)
                             [(aget since i) (aget to i) (aget tod i) (aget sd i)]
                             [(double (- i (nth starts j))) (double (mod j 4)) (double (mod j 8)) (double (mod j 16))
                              (/ (double i) (double (max 1 (dec n)))) (double (- n i))])))))
          (range n))))

(defn qlc-ml-fine-events
  "R526_FINE: Time-line's fine events as the model hears them - [[beat laengde \"pre\"|\"dip\"|\"out\"] ...]
   (1-baserede beats). pre: kun hvor et drop begynder lige efter; dip: kun inde i et drop, hoejst 8 beats"
  [rows starts states probs]
  (when-let [model @qlc-ml-fine-model]
    (let [X    (qlc-ml-fine-features rows starts states probs)
          cls  (:classes model)
          n    (count X)
          ;; R528_GROOVE_DIP: [klasse sandsynlighed] pr. beat - et dip i en groove skal vaere sikrere (0.8)
          seqv (mapv (fn [x]
                       (let [p (qlc-ml-proba model x)]
                         (reduce (fn [[bc bv] k]
                                   (let [c (nth cls k) th (get qlc-ml-fine-classes-used c)
                                         v (nth p k)]
                                     (if (and th (>= v th) (> v bv)) [c v] [bc bv])))
                                 ["NONE" -1.0] (range (count cls)))))
                     X)
          bar-of (fn [i] (dec (count (take-while #(<= % i) starts))))
          state  (fn [i] (nth states (max 0 (bar-of i))))
          drop-start? (fn [i] (and (= "DROP" (state i)) (or (zero? i) (not= "DROP" (state (dec i))))))
          ;; R528_GROOVE_RUNS: [klasse start laengde hoejeste-sandsynlighed]
          runs   (loop [i 0 acc []]
                   (if (>= i n) acc
                     (let [c (first (nth seqv i))]
                       (if (= c "NONE") (recur (inc i) acc)
                         (let [j (loop [j i] (if (and (< j n) (= (first (nth seqv j)) c)) (recur (inc j)) j))]
                           (recur j (conj acc [c i (- j i) (reduce max (map #(second (nth seqv %)) (range i j)))])))))))]
      (vec (keep (fn [[c s l mx]]
                   (case c
                     "PRE" (let [e (+ s l)]
                             (when (some drop-start? (range (max 0 (dec e)) (min n (+ e 4))))
                               [(inc s) l "pre"]))
                     ;; R528_GROOVE_KEEP (Tobias 11-02, forslag 5): ogsaa et dip i en groove (Time-line's
                     ;; drive-break), men kun hvor modellen er sikker (0.8: rammer 6 af 10 mod 7 af 10 i drops)
                     "DIP" (when (and (<= l 8) (or (= "DROP" (state s))
                                                   (and (= "GROOVE" (state s)) (>= mx 0.8))))
                             [(inc s) l "dip"])
                     "OUT" [(inc s) l "out"]
                     nil))
                 runs)))))

(defn qlc-ml-analyse
  "R526: sektionerne og de fine haendelser i én gennemgang - {:markers [...] :events [...]}, eller nil"
  [curves grid]
  (when-let [model @qlc-ml-model]
    (let [{:keys [starts X]} (qlc-ml-features curves grid)
          probs (mapv #(qlc-ml-proba model %) X)
          path  (qlc-ml-viterbi probs (count (:classes model)))]
      (when path
        (let [states (mapv #(nth (:classes model) %) path)
              n      (long (.-beatCount grid))]
          {:markers (qlc-ml-markers-of curves n starts states)
           ;; R528_CONFBARS: hvor sikker modellen er pr. takt - [[foerste beat sandsynlighed] ...]
           :conf    (mapv (fn [j s] [(inc s) (/ (Math/round (* 100.0 (double (nth (nth probs j) (nth path j))))) 100.0)])
                          (range (count starts)) starts)
           :events  (try (qlc-ml-fine-events (qlc-ml-beat-rows curves n) starts states probs)
                         (catch Throwable ex
                           (try (timbre/warn "QLC+ AI details failed:" (.getMessage ex)) (catch Throwable _ nil))
                           nil))})))))

;; titel -> {:grid gsig :markers [...] :events [...]}; kun i hukommelsen (regnes igen efter en genstart)
(defonce qlc-ml-sets (atom {}))

(defn qlc-ml-known
  "det laerte saet der allerede er regnet for titlen paa dette grid, eller nil"
  [title gsig]
  (let [e (get @qlc-ml-sets title)]
    (when (and e (= (:grid e) gsig)) (:markers e))))

(defn qlc-ml-events-known
  "R526_EVENTS_KNOWN: de fine haendelser regnet sammen med titlens laerte sektioner, eller nil"
  [title gsig]
  (let [e (get @qlc-ml-sets title)]
    (when (and e (= (:grid e) gsig)) (:events e))))

(defn qlc-ml-conf-known
  "R528_KNOWNCONF: modellens sikkerhed pr. takt for titlen paa dette grid, eller nil"
  [title gsig]
  (let [e (get @qlc-ml-sets title)]
    (when (and e (= (:grid e) gsig)) (:conf e))))

;; ---- R528_HAND (Tobias 11-02, forslag 8 med 2): hans rettelser bliver traeningsdata. Pr. haandrettet
;; nummer skrives modellens raekker (det BLT selv har: kurverne pr. beat) og hans flag til ~/.qlc-ml-hand.edn
;; (en linje pr. rettelse, kun tilfoejet, hoejst 60 MB). SEND TO CLAUDE tager filen med (BLT cache) - saa
;; kan modellen laere hvor HAN saetter drop og break. Intet sendes nogen steder fra BLT.
(def qlc-ml-hand-file (str (System/getProperty "user.home") "/.qlc-ml-hand.edn"))
(defonce qlc-ml-hand-logged (atom #{}))
(defonce qlc-ml-current (atom nil))       ; [titel grid kurver] for nummeret der sidst blev sendt

(defn qlc-ml-hand-log!
  [title grid curves markers]
  (let [sig [title (vec (sort (map (fn [x] [(long (:beat x)) (str (:type x))]) markers)))]]
    (when (and title grid curves (seq markers) (not (contains? @qlc-ml-hand-logged sig)))
      (try
        (let [f (java.io.File. ^String qlc-ml-hand-file)]
          (when (< (.length f) (* 60 1024 1024))
            (let [n      (long (.-beatCount grid))
                  rows   (qlc-ml-beat-rows curves n)
                  line   (pr-str {:title title :at (System/currentTimeMillis) :n n
                                  :starts (qlc-ml-bar-starts grid n)
                                  :markers (second sig)
                                  :rows (mapv (fn [^doubles r] (mapv #(/ (Math/round (* 1000.0 (double %))) 1000.0) r)) rows)})]
              (locking qlc-ml-hand-file
                (spit f (str line "\n") :append true))
              (swap! qlc-ml-hand-logged conj sig))))
        (catch Throwable ex
          (try (timbre/warn "QLC+ hand rows not written:" (.getMessage ex)) (catch Throwable _ nil)))))))

(defn qlc-ml-hand-later!
  "en rettelse fra QLC+: nummeret paa decket (det sidst sendte) skrives med de nye flag"
  [title markers]
  (when-let [[t grid curves] @qlc-ml-current]
    (when (= t title)
      (future (qlc-ml-hand-log! title grid curves markers)))))

(defn qlc-ml-for!
  "R525_ML: de laerte sektioner for en titel paa dens grid, regnet én gang (curves-fn giver kurverne).
   nil for en blaa boelgeform, uden model, eller naar den ikke finder et drop - saa bruger QLC+ vores."
  [title grid gsig curves-fn]
  (let [e (get @qlc-ml-sets title)]
    (if (and e (= (:grid e) gsig))
      (:markers e)
      ;; R526_FOR_BOTH: sektionerne og de fine haendelser i samme gennemgang
      (let [m (try (let [c (curves-fn)]
                     (when-not (:mono c) (qlc-ml-analyse c grid)))
                   (catch Throwable ex
                     (try (timbre/warn "QLC+ AI sections failed for" (pr-str title) "-" (.getMessage ex))
                          (catch Throwable _ nil))
                     nil))]
        (when title
          (swap! qlc-ml-sets (fn [s] (assoc (if (> (count s) 400) {} s) title
                                            ;; R528_KEEPCONF: modellens sikkerhed gemmes med
                                            {:grid gsig :markers (:markers m) :events (:events m) :conf (:conf m)}))))
        (:markers m)))))

;; modellen laeses i baggrunden naar filen indlaeses, ikke paa det foerste nummer
(future (try @qlc-ml-model @qlc-ml-fine-model (catch Throwable _ nil)))   ; R526_FINE_WARM

;; ---------------------------------------------------------------- afsendelse

(defn qlc-title
  "\"Artist - Title\" from rekordbox, or nil while the metadata is still on
   its way (nothing is analysed or cached under a placeholder name)."
  [player]
  (try
    (let [mf (org.deepsymmetry.beatlink.data.MetadataFinder/getInstance)
          md (.getLatestMetadataFor mf (int player))]
      (when md
        (let [t (.getTitle md)
              ;; SearchableItem carries the name in its public field `label`
              a (try (.-label (.getArtist md)) (catch Throwable _ nil))]
          (when (seq t)
            (if (and a (seq a)) (str a " - " t) t)))))
    (catch Throwable _ nil)))

;; runde 414 (aftenen 10-09): 7 numre naaede aldrig QLC+, selv om BLT havde
;; titlen - hvad der manglede, stod ingen steder. En linje pr. nummer i BLT's log.
(defonce qlc-missing-logged (atom #{}))

(defn qlc-send-track! [player]
  (try
    (let [wf     (org.deepsymmetry.beatlink.data.WaveformFinder/getInstance)
          bg     (org.deepsymmetry.beatlink.data.BeatGridFinder/getInstance)
          detail (.getLatestDetailFor wf (int player))
          grid   (.getLatestBeatGridFor bg (int player))
          title  (qlc-title player)]
      ;; no analysis until the metadata is in and QLC+ can be reached - a
      ;; failed send would otherwise redo the whole pass five times a second
      (if (and detail grid title (qlc-writer!))
        (let [curves  (qlc-curves detail grid)
              total   (try (.getTotalTime detail) (catch Throwable _ nil))
              cached  (qlc-cache-get title total)
              entry   (get @qlc-cache title)
              ;; haandrettede flag er sandheden, og vores egen analyse i DENNE
              ;; version staar (ogsaa en klargjort paa det andet deck). Alt
              ;; andet regnes om: en aeldre version, og rekordbox' faser gemt
              ;; foer analyse 10 - deres typer var dem der ikke holdt. Kun
              ;; finderens cache her (ask? false): det her er triggeren, og et
              ;; netvaerkskald ville holde positionerne tilbage.
              hand?   (boolean (and cached (qlc-hand-made? entry)))
              ;; the grid the flags were counted on (re-gridded = same title and length)
              gsig    [(.-beatCount grid) (qlc-downbeat grid (.-beatCount grid))]
              ours?   (and cached (not hand?) (not (:rekordbox entry))
                           (= (:version entry) qlc-analysis-version)
                           (= (:grid entry) gsig))
              markers (if (or hand? ours?)
                        cached
                        ;; qlc-cache-put! gemmer selv naar entry'en er en der
                        ;; skal ligge paa disken (runde 209)
                        (let [m (qlc-auto-markers player detail grid curves false)]
                          ;; runde 295 (T2): the grid these flags were counted on
                          ;; - in the same write (B22)
                          (qlc-cache-put! title (qlc-with-grid m gsig) false total)
                          m))
              ;; the kick analysis (analyse 10), not one of its fallbacks
              kick?   (boolean (if (or hand? ours?) (:kick entry) (:kick (meta markers))))
              ;; R525_SENDAI: de laerte sektioner ved siden af (ikke til haandrettede numre)
              ai      (when-not hand? (qlc-ml-for! title grid gsig (constantly curves)))
              ;; R528_HAND_SEND: nummeret paa decket (til en rettelse), og et haandrettet nummer skrives
              ;; som traeningsdata (én gang pr. aften og flag-saet)
              _       (do (reset! qlc-ml-current [title grid curves])
                          (when hand? (future (qlc-ml-hand-log! title grid curves cached))))
              display (:display curves)
              ok      (qlc-send! {:evt      "track"
                                  :title    title
                                  ;; the deck: the same title from the OTHER deck is
                                  ;; a new play, not a resend (runde 188)
                                  :player   (int player)
                                  :key      (qlc-key player)
                                  :bpm      (try (/ (.getBpm grid (int 1)) 100.0)
                                                 (catch Throwable _ 0.0))
                                  :beats    (count display)
                                  ;; where the first bar starts (0 = beat 1): QLC+ snaps the
                                  ;; flags the operator sets to the track's own bar lines,
                                  ;; not to beat 1 mod 4 (BACKLOG 104, analyse 10b)
                                  :downbeat (qlc-downbeat grid (count display))
                                  :duration (try (.getTotalTime detail)
                                                 (catch Throwable _ 0))
                                  :waveform (mapv #(int (* 255 (max 0.0 (min 1.0 %))))
                                                  display)
                                  ;; what the analysis saw, for the Track page's overlay
                                  :low      (mapv #(int (* 255 (max 0.0 (min 1.0 %)))) (:low curves))
                                  :high     (mapv #(int (* 255 (max 0.0 (min 1.0 %)))) (:high curves))
                                  :kick     (mapv #(int (* 255 (max 0.0 (min 1.0 %)))) (:kick curves))
                                  ;; runde 323: kick og top slag for slag, uglattet - QLC+ finder
                                  ;; vendingerne (kicket tilbage efter et hul, et crash) i dem. I de
                                  ;; glattede kurver ramte kick-vendingen slag 1 i 35 % af tilfaeldene,
                                  ;; i de uglattede 71 % (92 numre fra biblioteket)
                                  :kickRaw  (mapv #(int (* 255 (max 0.0 (min 1.0 %)))) (:kick-raw curves))
                                  :highRaw  (mapv #(int (* 255 (max 0.0 (min 1.0 %)))) (:high-raw curves))
                                  ;; runde 358: mellemtonen og toppens anslag pr. fjerdedel af beatet
                                  ;; (fire nibbles) - stemmen alene, klap paa 2/4, off-beat-hatten.
                                  ;; Ikke fra en blaa (mono) boelgeform: dens baand er gaettet.
                                  :midRaw   (if (:mono curves) [] (mapv #(int (* 255 (max 0.0 (min 1.0 %)))) (:mid-raw curves)))
                                  :onsetHigh (if (:mono curves) [] (vec (:onset-high curves)))
                                  ;; R461_ONSETS: bassen (ide 2: bassen paa og'et) og mellemtonen
                                  ;; (ide 3: plucks og percussion i breaks) pr. fjerdedel
                                  :onsetLow  (if (:mono curves) [] (vec (or (:onset-low curves) [])))
                                  :onsetMid  (if (:mono curves) [] (vec (or (:onset-mid curves) [])))
                                  ;; R493_A1_BLT_SEND (runde 493): rekordbox' genre-tag (kun et
                                  ;; svagt hint), swingen (0 lige .. 1 fuld shuffle, null = ukendt:
                                  ;; den samlede, sekstendedelenes og ottendedelens) og mellemtonens
                                  ;; stabs pr. fjerdedel. En aeldre QLC+ ser bort fra dem.
                                  :genre    (or (qlc-genre player) "")
                                  :swing    (:swing (:swing curves))
                                  :swing16  (:swing16 (:swing curves))
                                  :swing8   (:swing8 (:swing curves))
                                  :stabMid  (if (:mono curves) [] (vec (or (:onset-stab curves) [])))
                                  ;; R521_ROLL32_SEND (runde 521, Tobias 10-31): de skaeve
                                  ;; toogtredivtedeles anslag pr. sekstendedel (fire nibbles som
                                  ;; onsetHigh, 0 = ikke spillet) - QLC+'s build-hvirvel ruller
                                  ;; kun paa 32-dele hvor musikken spiller dem. Ny kurve, samme
                                  ;; flag: kurverne regnes forfra ved hver sending (kun flagene
                                  ;; caches), saa qlc-analysis-version staar. En aeldre QLC+ ser
                                  ;; bort fra feltet.
                                  :onset32  (if (:mono curves) [] (vec (or (:onset-32 curves) [])))
                                  ;; runde 359: hvor dybt lyden falder mellem kickene (kun hoejden -
                                  ;; ogsaa fra en blaa boelgeform)
                                  :pumpRaw  (mapv #(int (* 255 (max 0.0 (min 1.0 %)))) (:pump-raw curves))
                                  ;; runde 317: kick og bas i absolutte tal (se qlc-curves)
                                  ;; ... men ikke fra en blaa (mono) boelgeform: dens baand er gaettet
                                  ;; (qlc-measure-beat), saa tallene kan ikke sammenlignes med farvede
                                  :kickRef  (if (:mono curves) -1.0 (double (or (:kick-ref curves) -1.0)))
                                  :lowRef   (if (:mono curves) -1.0 (double (or (:low-ref curves) -1.0)))
                                  ;; hand-made flags are the truth; QLC+ leaves them alone.
                                  ;; Only those: rekordbox' phrases went as manual too, and
                                  ;; that switched the engine's kick corrections off for
                                  ;; every rekordbox track (BACKLOG 106) - they are part of
                                  ;; our analysis now, not a correction of it
                                  :manual   hand?
                                  ;; hvor opdelingen kom fra, saa Track-siden kan sige det
                                  :source   (if hand? "hand" "auto")
                                  ;; analyse 10+: sektionerne er allerede maalt paa kicket
                                  ;; og ligger paa nummerets egne taktlinjer - QLC+ springer
                                  ;; sin anden gennemgang over (den slettede breaks under fire
                                  ;; takter og flyttede flag til beat 1 mod 4)
                                  :analysis (cond hand? 0 kick? qlc-analysis-version :else 9)
                                  :markers  markers
                                  ;; R525_MAPAI: Time-line-modellens sektioner - QLC+ vaelger mellem dem
                                  ;; og :markers (SETUP > ADVANCED); en aeldre QLC+ ser bort fra dem
                                  :markersAi  (or ai [])
                                  :analysisAi qlc-ml-analysis
                                  ;; R526_SEND_EVENTS: de fine haendelser [[beat laengde "pre"|"dip"|"out"] ...] -
                                  ;; QLC+ bruger dem kun med AI DETAILS slaaet til
                                  :events     (if ai (or (qlc-ml-events-known title gsig) []) [])
                                  ;; R528_SENDCONF: modellens sikkerhed pr. takt (QLC+: ingen hvid landing paa et
                                  ;; usikkert drop; skraveringen paa Track-siden)
                                  :aiConf     (if ai (or (qlc-ml-conf-known title gsig) []) [])
                                  ;; runde 357: rekordbox' fill-ins (qlc-phrase-fills)
                                  :fills    (qlc-phrase-fills player)})]
          ;; a full queue after a whole analysis is worth the back-off (r199)
          (when-not ok
            (swap! qlc-failed-at assoc [:send player title] (System/currentTimeMillis)))
          (try (timbre/info "QLC+ track" (if ok "sent" "FAILED") "-"
                            (count display) "beats," (count markers) "markers"
                            (let [pc (:patent (get @qlc-cache title))]
                              (if pc (str "(" pc " patent cuts)") ""))
                            ;; R525_LOGAI
                            (if (seq ai) (str "+ AI " (count ai) " sections") "")
                            (cond hand? "(hand-made)"
                                  ours? "(cached)"
                                  :else (str "(analysed v" qlc-analysis-version ")")))
               (catch Throwable _ nil))
          ok)
        (do
          ;; runde 414: say once per track WHAT is missing (BLT's log)
          (when title
            (let [k [(long player) title (nil? detail) (nil? grid)]]
              (when-not (contains? @qlc-missing-logged k)
                (swap! qlc-missing-logged conj k)
                (try (timbre/warn "QLC+ cannot send" (pr-str title) "from player" player "- missing:"
                                  (clojure.string/join ", "
                                    (cond-> []
                                      (nil? detail)           (conj "waveform detail")
                                      (nil? grid)             (conj "beat grid")
                                      (and detail grid)       (conj "connection to QLC+"))))
                     (catch Throwable _ nil)))))
          false)))
    (catch Throwable e
      ;; the back-off is for THIS - a throw. "Not ready yet" (no waveform or
      ;; grid for a second after a load) also returns false, and a 5 s wait on
      ;; that held the new track back while its beats ran on the old flags
      ;; (runde 198)
      (try (swap! qlc-failed-at assoc [:send player (qlc-title player)] (System/currentTimeMillis))
           (catch Throwable _ nil))
      (timbre/error e "QLC+ send-track failed for player" player)
      false)))

(defn qlc-deck-tempo
  "The effective tempo of the deck QLC+ follows - the track's BPM with the
   pitch fader in it - read off its latest status. nil when unknown. A READ:
   nothing goes back to the players (REGLER: vi laeser, vi skriver aldrig)."
  []
  (try
    (let [[p _] @qlc-sent-track
          st    (when p (.getLatestStatusFor (org.deepsymmetry.beatlink.VirtualCdj/getInstance) (int p)))
          t     (when st (.getEffectiveTempo st))]
      (when (and t (> (double t) 20.0) (< (double t) 400.0))
        (/ (Math/round (* 100.0 (double t))) 100.0)))
    (catch Throwable _ nil)))

;; LAST_TRACK_FRESH_MIXER: closure uses a single read-only snapshot. A mixer seen
;; earlier is not proof it is still reporting; stale/missing/failed status is unknown.
;; Include decks 5/6 when BLT supplies them. This does not implement a new DJ protocol.
(defonce qlc-closure-epoch (System/nanoTime))
(def qlc-closure-fresh-ns 1500000000)
(defn qlc-closure-state []
  (try
    (when @qlc-mixer-seen
      (let [vc (org.deepsymmetry.beatlink.VirtualCdj/getInstance)
            now (System/nanoTime)
            fresh? (fn [s] (and s (let [age (- now (.getTimestamp s))]
                                    (<= 0 age qlc-closure-fresh-ns))))
            mixer (first (sort-by #(.getTimestamp %) >
                           (filter #(and (instance? org.deepsymmetry.beatlink.MixerStatus %)
                                         (.startsWith (.getDeviceName %) "DJM") (fresh? %))
                                   (.getLatestStatus vc))))
            statuses (into {} (keep (fn [p] (when-let [s (.getLatestStatusFor vc (int p))] [p s]))
                                   (range 1 7)))
            [followed _] @qlc-sent-track
            source-ms (when mixer (quot (- (.getTimestamp mixer) qlc-closure-epoch) 1000000))]
        (when (and mixer (some? source-ms) (>= source-ms 0)
                   (instance? org.deepsymmetry.beatlink.CdjStatus (get statuses followed))
                   (every? #(and (instance? org.deepsymmetry.beatlink.CdjStatus %) (fresh? %))
                           (vals statuses)))
          (let [on-air (filter #(.isOnAir ^org.deepsymmetry.beatlink.CdjStatus %) (vals statuses))]
            {:air (boolean (or (.isOnAir ^org.deepsymmetry.beatlink.CdjStatus (get statuses followed))
                               (some #(.isPlaying ^org.deepsymmetry.beatlink.CdjStatus %) on-air)))
             :otherHeard (boolean (some #(and (not= followed (.getDeviceNumber %))
                                             (.isPlaying ^org.deepsymmetry.beatlink.CdjStatus %)) on-air))
             :faders (count on-air)
             :mixerTime source-ms}))))
    (catch Throwable _ nil)))

;; Compatibility helpers: unknown remains nil, never false/zero.
(defn qlc-deck-air [] (:air (qlc-closure-state))) ; R479_LAST_WHITE_CLJAIR
(defn qlc-deck-faders [] (:faders (qlc-closure-state))) ; R484_LF_CLJFADERS

;; A false callback after a device disappeared is not a confirmed stop.
(defn qlc-deck-stopped? []
  (try
    (let [[p _] @qlc-sent-track
          s (when p (.getLatestStatusFor (org.deepsymmetry.beatlink.VirtualCdj/getInstance) (int p)))
          age (when s (- (System/nanoTime) (.getTimestamp s)))]
      (and (instance? org.deepsymmetry.beatlink.CdjStatus s)
           (<= 0 age qlc-closure-fresh-ns)
           (not (.isPlaying ^org.deepsymmetry.beatlink.CdjStatus s))))
    (catch Throwable _ false)))

;; TRACK_APP_CDJLOOP_R582: {:loopOk true :loop [first-beat last-beat]} while the followed deck plays a loop,
;; {:loopOk true} when it can report loops and plays none, nil when it cannot (an older player) or on any doubt
(defn qlc-deck-loop []
  (try
    (let [[p _] @qlc-sent-track
          s (when p (.getLatestStatusFor (org.deepsymmetry.beatlink.VirtualCdj/getInstance) (int p)))]
      (when (and (instance? org.deepsymmetry.beatlink.CdjStatus s)
                 (.canReportLooping ^org.deepsymmetry.beatlink.CdjStatus s))
        (let [^org.deepsymmetry.beatlink.CdjStatus st s
              lend (.getLoopEnd st)
              grid (when (pos? lend)
                     (.getLatestBeatGridFor (org.deepsymmetry.beatlink.data.BeatGridFinder/getInstance) (int p)))
              b0   (when grid (.findBeatAtTime grid (long (+ (.getLoopStart st) 5))))
              b1   (when grid (.findBeatAtTime grid (long (max 0 (- lend 5)))))]
          (if (and (.isLooping st) b0 b1 (pos? (int b0)) (>= (int b1) (int b0)))
            {:loopOk true :loop [(int b0) (int b1)]}
            {:loopOk true}))))
    (catch Throwable _ nil)))

(defn qlc-send-position! [beat playing?]
  ;; a stop goes through whatever the beat says. This is the only event that
  ;; carries :playing false, and a deck paused where the position is unknown
  ;; used to send nothing at all - QLC+ kept animating the last look.
  ;; runde 301 (Tobias: "Moteren skal vel bruge live BPM fra cdj?"): and the
  ;; deck's effective tempo, so QLC+ times its milliseconds on the pitch the
  ;; DJ is playing - Link is used first when QLC+ has it (qlc-deck-tempo)
  (when (and beat (or (not playing?) (pos? (int beat))))
    (let [bpm (qlc-deck-tempo)
          closure (qlc-closure-state)   ; LAST_TRACK_FRESH_MIXER: one consistent snapshot
          air (:air closure)
          faders (:faders closure)]
      ;; LAST_TRACK_CONFIRMED_STOP: device loss cannot fabricate a stop, and a
      ;; playing/on-air deck 5/6 protects the room just like another deck 1-4.
      (when (or playing? (and (qlc-deck-stopped?) (not (:otherHeard closure))))
        (qlc-send! (cond-> {:evt "pos" :beat (max 0 (int beat)) :playing (boolean playing?)}
                   bpm (assoc :bpm bpm)
                   (some? air) (assoc :air air)
                   (some? faders) (assoc :faders faders)
                   closure (assoc :mixerTime (:mixerTime closure))
                   true (merge (qlc-deck-loop))))))))   ; R484_LF_CLJPOS2 / TRACK_APP_CDJLOOP_R582

;; ---------------------------------------------------------------- wiring

;; runde 413 (Tobias 10-09, live): see qlc-fallback-track! below
(def qlc-fallback-ms 4000)
(defonce qlc-fallback-sent (atom #{}))   ; [player sig] QLC+ has as a BARE track
(defonce qlc-nodata-since (atom nil))    ; [[player sig] since-ms]

(defn qlc-maybe-send-track! [player]
  (let [sig (qlc-title player)
        k   [:send player sig]]
    (when (and (or (not= sig (get @qlc-last-track player))
                   (and sig (not= [player sig] @qlc-sent-track))
                   ;; runde 413: a bare track is replaced by the real one the
                   ;; moment its data is in
                   (some #(= (first %) (long player)) @qlc-fallback-sent))
               (> (- (System/currentTimeMillis) (get @qlc-failed-at k 0)) 5000))
      ;; the socket before the send: if QLC+ restarted meanwhile, the track
      ;; went to the dead one, and it must not be recorded as sent (runde 197)
      ;; (s nil = no connection yet: the writer connects and then sends, so it
      ;; arrives - recorded, or it went out once more on the next beat, r198)
      (let [s (:socket @qlc-conn)]
        (when (qlc-send-track! player)
          (when (or (nil? s) (identical? s (:socket @qlc-conn)))
            (reset! qlc-sent-track [player sig])
            (swap! qlc-last-track assoc player sig)
            (swap! qlc-fallback-sent (fn [sent] (set (remove #(= (first %) (long player)) sent))))))))))

;; ---------------------------------------------------------------- the deck the room hears
;;
;; runde 203 (Tobias, 2026-09-24): "Lyset skal foelge det deck publikum
;; hoerer." The trigger watches the TEMPO MASTER, and the master is not always
;; what the room hears: handed to B before B's fader is up, the lights showed
;; B's flags while the room heard A; left on a stopped A, the room went idle
;; while B played.
;;
;; Heard = playing and on air (or just playing, when BLT has seen no mixer -
;; the same rule as qlc-mixing?). When more than one deck is heard (a mix),
;; the tempo master decides, as it always did - the mix logic in QLC+ is built
;; round that moment. A new choice has to hold for qlc-follow-hold-ms, so a
;; fader dip or a cut through the channel does not swap tracks back and forth.

(def qlc-follow-hold-ms 2000)
(def qlc-followed (atom nil))           ; the player the lights follow
(def qlc-follow-candidate (atom nil))   ; [player since-ms]

(defn qlc-tempo-master []
  (try (some-> (org.deepsymmetry.beatlink.VirtualCdj/getInstance)
               .getTempoMaster .getDeviceNumber long)
       (catch Throwable _ nil)))

(defn qlc-heard-decks
  "The players the room hears, as a set of longs."
  []
  (try
    (let [vc (org.deepsymmetry.beatlink.VirtualCdj/getInstance)]
      (set (for [p [1 2 3 4]
                 :let [st (.getLatestStatusFor vc (int p))]
                 :when (and (instance? org.deepsymmetry.beatlink.CdjStatus st)
                            (.isPlaying ^org.deepsymmetry.beatlink.CdjStatus st)
                            (or (not @qlc-mixer-seen)
                                (try (.isOnAir ^org.deepsymmetry.beatlink.CdjStatus st)
                                     (catch Throwable _ false))))]
             (long p))))
    (catch Throwable _ #{})))

(defn qlc-heard-choice
  "Heard and master: the master. Else the deck already followed, while it is
   still heard. Else any deck that is heard. Nothing heard (a pause, faders
   down): stay where we are. Nothing followed yet either: the master, but
   only a player 1-4 - BLT itself or a mixer as tempo master is no deck, and
   following it cost the 2 s hold before the first heard deck (runde 218)."
  [master heard followed]
  (cond
    (and master (contains? heard master))     master
    (and followed (contains? heard followed)) followed
    (seq heard)                               (apply min heard)
    :else                                     (or followed
                                                  (when (and master (<= 1 master 4)) master))))

;; runde 360 (Tobias: "mixet som musik"): in a mix the lights follow the deck
;; the room HEARS FILL - when the other deck's drop lands while the followed
;; one is in a break, an intro or an outro, the drop takes the lights at once,
;; not when the DJ gets round to handing MASTER over. 10-02: 56 of 281 drop
;; landings were inside a mix, and 28 of 95 handovers came after the incoming
;; drop had already landed. Only read - nothing is sent to the decks.
(declare qlc-drop-take)   ; defined after qlc-position-of, below

(defonce qlc-follow-master (atom nil))   ; the tempo master when the lights took their deck (fejljagt 3)

(defn qlc-follow!
  "The player to follow now, given the trigger's player (the tempo master)."
  [player]
  (let [master (or (qlc-tempo-master) (long player))
        heard  (qlc-heard-decks)
        cur    @qlc-followed
        ;; the master wins over a followed deck that is still heard only when
        ;; it has CHANGED since that deck was taken - a handover. A master left
        ;; on a stopped deck from the last mix won the moment its fader came up
        ;; again: the lights jumped to the incoming track 2 s into the mix,
        ;; while the room still heard the outgoing one (fejljagt 3)
        master* (if (and cur (not= cur master) (= master @qlc-follow-master)) nil master)
        take   (when cur (qlc-drop-take cur heard))          ; runde 360
        want   (or take (qlc-heard-choice master* heard cur))
        now    (System/currentTimeMillis)
        [cp since] @qlc-follow-candidate]
    (cond
      (nil? want)   nil
      (nil? cur)    (do (reset! qlc-followed want) (reset! qlc-follow-master master)
                        (reset! qlc-follow-candidate nil))
      (== want cur) (reset! qlc-follow-candidate nil)
      ;; runde 360: the incoming drop takes the room - at once, as a handover
      (and take (== want take))
      (do (reset! qlc-followed want)
          (reset! qlc-follow-master master)
          (reset! qlc-follow-candidate nil)
          (try (timbre/info "QLC+: lights follow deck" want "(its drop landed in the mix)")
               (catch Throwable _ nil)))
      ;; R479_CUE_DECK_2 (night review 10-17): MASTER handed to the other deck just
      ;; as ours landed its drop. The drop rule took the lights straight back on
      ;; the next update - 02:07:10 Ben Hemsley for 90 ms, two full track changes
      ;; and looks. Left exactly as that take-back left it: ours stays, the new
      ;; master is noted, the candidate goes - only no track is sent.
      (and (== want master) (contains? heard cur) @qlc-mixing
           (= cur (qlc-drop-take want heard)))
      (do (reset! qlc-follow-master master)
          (reset! qlc-follow-candidate nil))
      ;; the DJ handing MASTER over inside a running mix is deliberate, not a
      ;; fader dip: at once, as it always was
      (and (== want master) (contains? heard cur) @qlc-mixing)
      (do (reset! qlc-followed want)
          (reset! qlc-follow-master master)
          (reset! qlc-follow-candidate nil)
          (try (timbre/info "QLC+: lights follow deck" want "(master handed over in the mix)")
               (catch Throwable _ nil)))
      (or (nil? cp) (not= cp want)) (reset! qlc-follow-candidate [want now])
      ;; leaving a deck that is still heard is a mix: QLC+ has to be told it
      ;; is one FIRST, or setMixing() marks the NEW track as the one going out
      ;; (qlc-update-mixing! runs after this, so the swap comes one update later)
      (and (>= (- now since) qlc-follow-hold-ms)
           (or (not (contains? heard cur)) @qlc-mixing))
      (do (reset! qlc-followed want)
          (reset! qlc-follow-master master)
          (reset! qlc-follow-candidate nil)
          (try (timbre/info "QLC+: lights follow deck" want
                            (if (== want master) "(tempo master)" "(heard - not the tempo master)"))
               (catch Throwable _ nil))))
    (or @qlc-followed (long player))))

(defn qlc-position-of
  "[beat playing?] for a player: the TimeFinder's (the beat number it keeps
   in step with beat packets), else the player's own last status."
  [p]
  (try
    (let [tf  (org.deepsymmetry.beatlink.data.TimeFinder/getInstance)
          pos (when (.isRunning tf) (.getLatestPositionFor tf (int p)))]
      ;; runde 414 (aftenen 10-09): four times the lights froze 30-50 s while
      ;; the deck played on - its beat came back 60-100 beats further. A
      ;; latest position older than 1.5 s is a deck the TimeFinder lost: the
      ;; deck's own status then (5 a second). Unknown field: as before.
      (if (and pos
               (try (< (- (System/nanoTime) (long (.-timestamp pos))) 1500000000)
                    (catch Throwable _ true)))
        [(.-beatNumber pos) (.-playing pos)]
        (let [st (.getLatestStatusFor (org.deepsymmetry.beatlink.VirtualCdj/getInstance) (int p))]
          (when (instance? org.deepsymmetry.beatlink.CdjStatus st)
            [(.getBeatNumber ^org.deepsymmetry.beatlink.CdjStatus st)
             (.isPlaying ^org.deepsymmetry.beatlink.CdjStatus st)]))))
    (catch Throwable _ nil)))

;; runde 360: the incoming drop takes the lights (see qlc-follow!)
(defn qlc-drop-take-choice
  "The other deck, when its drop has just landed (0-2 beats in) and the
   followed deck is KNOWN to be somewhere quieter - not a drop, not a drive;
   else nil. Pure. (runde 361: a followed track with no analysis in the cache
   read as 'not a drop' and handed the lights over in the middle of its own
   drop; a drive is a full section too.)"
  [cur cur-type other other-type other-into]
  (when (and other cur (not= other cur) (= other-type "drop")
             cur-type (not (contains? #{"drop" "drive"} cur-type))
             other-into (<= 0 other-into 2))
    other))

(defn qlc-marker-at
  "The cached section a title is in at a beat, or nil."
  [title beat]
  (when (and title beat (pos? beat))
    (when-let [ms (seq (qlc-cache-get title))]
      (last (sort-by :beat (filter #(<= (:beat %) beat) ms))))))

;; R479_CUE_DECK (night review 10-17, S8 finding 4): after 02:00 decks that were
;; only being cued took the lights. 02:21:35 WISH was hot-cued onto its drop flag
;; (beat 161) while Empire played its break, and the rule above read the jump as
;; a landing: a full track change and a DROP, then back. 02:28:34 Timeless looped
;; a bar (457-460) into its drop and took the lights back from the deck just
;; mixed in; 02:02:20 Nick Curly, cue-tapped at beat 1 (a drop flag), held them
;; 22 s. A drop takes the lights now only when its deck PLAYED into it:
;; qlc-drop-take-run-beats beats of play, no jump, before the flag. The real
;; takes that night came 4-52 s into the mix (and the mix itself needs 2 s), so
;; they keep landing on the drop beat, at once. Read off the same positions as
;; before - we only read, nothing goes to the decks.
(def qlc-drop-take-run-beats 8)
(defonce qlc-run-from (atom {}))   ; player -> [first-beat last-beat] of its run of play

(defn qlc-run-step
  "Pure. A deck's run after a reading: kept while it plays on (one beat back -
   the status echo - up to a bar on since the last reading), begun afresh at a
   jump (a hot cue, a loop, a cue), nil when it stops or the beat is unknown."
  [run beat playing?]
  (when (and playing? beat (pos? (long beat)))
    (let [b           (long beat)
          [from last] run]
      (if (and from last (<= -1 (- b (long last)) 4))
        [from (max b (long last))]
        [b b]))))

(defn qlc-played-into?
  "Pure. The run reached the flag at drop-beat by playing - at least
   qlc-drop-take-run-beats beats of it before the flag."
  [run drop-beat]
  (let [[from _] run]
    (boolean (and from drop-beat
                  (<= (+ (long from) qlc-drop-take-run-beats) (long drop-beat))))))

(defn qlc-note-runs!
  "Every deck's run of play, from its position now (a read)."
  []
  (doseq [p [1 2 3 4]]
    (let [[b pl] (qlc-position-of p)
          r      (qlc-run-step (get @qlc-run-from p) b pl)]
      (if r
        (swap! qlc-run-from assoc p r)
        (swap! qlc-run-from dissoc p)))))

(defn qlc-drop-take
  "In a mix with ONE other heard deck: that deck, if its drop just landed and
   ours is not in one (qlc-drop-take-choice), and it played into that drop
   (R479_CUE_DECK); else nil."
  [cur heard]
  (try
    (qlc-note-runs!)                    ; every update, mix or not: a run starts before the mix
    (let [others (disj (set heard) cur)]
      (when (and @qlc-mixing cur (= 1 (count others)))
        (let [o       (first others)
              [ob _]  (qlc-position-of o)
              [cb _]  (qlc-position-of cur)
              om      (qlc-marker-at (qlc-title o) ob)
              cm      (qlc-marker-at (qlc-title cur) cb)
              t       (qlc-drop-take-choice cur (:type cm) o (:type om)
                                            (when (and om ob) (- (long ob) (long (:beat om)))))]
          ;; R479_CUE_DECK: a drop it played into, not one it was cued onto
          (when (and t (qlc-played-into? (get @qlc-run-from (long t)) (:beat om)))
            t))))
    (catch Throwable _ nil)))

(defn qlc-on-beat
  "A beat packet from any player. Sent on only when it is the followed deck,
   its track is the one QLC+ has, and it is NOT the tempo master (the
   trigger's qlc-beat! has those). BeatFinder tells the TimeFinder first, so
   its beat number is already this beat's."
  [beat]
  (try
    (let [dev   (long (.getDeviceNumber ^org.deepsymmetry.beatlink.Beat beat))
          f     @qlc-followed
          [p _] @qlc-sent-track
          m     (qlc-tempo-master)]
      ;; the TimeFinder's number only: the player's own status often arrives
      ;; after the beat packet and still says the beat before - without a
      ;; TimeFinder the status updates carry this deck instead
      ;; ... and the TITLE (fejljagt 09-27): runde 249 closed this in
      ;; qlc-tracked-update! and qlc-beat!, not here - a new track on the
      ;; followed deck whose send failed still matched on the deck alone
      (when (and f p (== dev f) (== p f) (or (nil? m) (not= m f))
                 (= (qlc-title f) (second @qlc-sent-track)))
        (let [tf  (org.deepsymmetry.beatlink.data.TimeFinder/getInstance)
              pos (when (.isRunning tf) (.getLatestPositionFor tf (int f)))]
          (when pos
            (qlc-send-position! (.-beatNumber pos) true)))))
    (catch Throwable _ nil)))

;; defonce: re-evaluating the shared functions must not add a second listener.
;; The reify calls qlc-on-beat through its var, so a new definition is used.
(defonce qlc-beat-listener (atom nil))

(defn qlc-follow-beats! []
  (when (nil? @qlc-beat-listener)
    (try
      (let [l (reify org.deepsymmetry.beatlink.BeatListener
                (newBeat [_ beat] (qlc-on-beat beat)))]
        (.addBeatListener (org.deepsymmetry.beatlink.BeatFinder/getInstance) l)
        (reset! qlc-beat-listener l))
      (catch Throwable e
        (try (timbre/warn "QLC+ could not listen for beats:" (.getMessage e))
             (catch Throwable _ nil))))))

;; ---------------------------------------------------------------- runde 413: no track data
;;
;; Tobias 10-09, live: "Det er blt der ikke laeser nummeret ind - lyset virker
;; heller ikke." Beats only go to QLC+ once its track has (runde 230/249), so a
;; deck whose title, beat grid or waveform never arrives in BLT left QLC+ with
;; NOTHING: four seconds later it read the link as lost and the look fell. Now
;; a followed deck that has played for qlc-fallback-ms with nothing QLC+ can
;; use goes out as a BARE track - its title (or "Deck N - no track data"), its
;; tempo, no beats, no flags - and its beats follow. QLC+ runs it as one long
;; groove on a 64-beat grid; the SECTION tiles steer it by hand. The real track
;; still goes out the moment its data is in (qlc-maybe-send-track!). Only read
;; from the decks - nothing goes back to them.
(defn qlc-fallback-track!
  [player]
  (try
    (let [p            (long player)
          sig          (qlc-title p)
          k            [p sig]
          now          (System/currentTimeMillis)
          [_ playing?] (qlc-position-of p)]
      (cond
        (or (not playing?) (= k @qlc-sent-track))
        (reset! qlc-nodata-since nil)

        (not= k (first @qlc-nodata-since))
        (reset! qlc-nodata-since [k now])

        (and (>= (- now (long (second @qlc-nodata-since))) qlc-fallback-ms)
             (qlc-writer!))
        (let [st  (.getLatestStatusFor (org.deepsymmetry.beatlink.VirtualCdj/getInstance) (int p))
              t   (try (when st (double (.getEffectiveTempo st))) (catch Throwable _ nil))
              bpm (if (and t (> t 20.0) (< t 400.0)) (/ (Math/round (* 100.0 t)) 100.0) 0.0)]
          (when (qlc-send! {:evt      "track"
                            :title    (or sig (str "Deck " p " - no track data"))
                            :player   (int p)
                            :bpm      bpm
                            :beats    0
                            :downbeat 0
                            :duration 0
                            :markers  []
                            :manual   false
                            :source   "fallback"})
            (reset! qlc-sent-track k)
            (swap! qlc-last-track assoc p sig)
            (swap! qlc-fallback-sent conj k)
            (reset! qlc-nodata-since nil)
            (try (timbre/warn "QLC+: no track data for player" p "- sent as a bare track:" (pr-str sig))
                 (catch Throwable _ nil))))

        :else nil))
    (catch Throwable _ nil)))

(defn qlc-tracked-update!
  ([player beat] (qlc-tracked-update! player beat true))
  ([player beat playing?]
   ;; foer alt andet: finderen skal have sin PSSI-lytter inden numrene bliver
   ;; lagt paa, ellers henter den ikke faserne (naar lytteren er sat: et
   ;; klasseopslag og to refleksive kald - billigt, men ikke gratis)
   (qlc-pssi-finder)
   (qlc-follow-beats!)
   ;; runde 203: the deck the room hears, not necessarily the trigger's
   (let [f (qlc-follow! player)
         ;; runde 205: the followed deck stopped while another is heard - a
         ;; pause before the drop, or the DJ cutting to the other deck. Its
         ;; "stopped" put QLC+'s idle look up (runde 204 switched at once
         ;; instead, and a one-bar pause in a mix swapped tracks and back,
         ;; each a full reset). Now the stop is held back: the look stays, and
         ;; after the 2 s hold the heard deck takes over - or the deck plays on.
         others-heard? (boolean (seq (disj (qlc-heard-decks) f)))]
     (qlc-maybe-send-track! f)
     (qlc-fallback-track! f)             ; runde 413: no data for 4 s - a bare track
     (if (== f (long player))
       ;; runde 230 (BACKLOG 66): a PLAYING beat only once this deck's track
       ;; is the one QLC+ has - a failed or not-yet-ready track send left QLC+
       ;; on the previous deck's flags, and these beats ran them (the other
       ;; branch and qlc-beat! already check this). A stop always goes out.
       ;; runde 249: the deck AND its title - a new track on the followed
       ;; deck whose send failed (title or grid not ready) still matched on
       ;; the player alone, and its beats ran the previous track's flags
       (when (and (or playing? (not others-heard?))
                  (or (not playing?) (= [f (qlc-title f)] @qlc-sent-track)))
         (qlc-send-position! beat playing?))
       ;; ... and only once its track is the one QLC+ has - its beat numbers
       ;; on the previous track ran that track's flags (runde 202's bug, which
       ;; any heard deck could hit now, not just the master)
       (when (= [f (qlc-title f)] @qlc-sent-track)
         (when-let [[b pl] (qlc-position-of f)]
           (when (or pl (not others-heard?))
             (qlc-send-position! b pl)))))
     (qlc-update-mixing!)
     (qlc-note-played!)
     (qlc-prepare-others! f)
     (qlc-send-mix-profile! f))))

;; runde 202: only the beat of the deck whose track QLC+ has. At a handover
;; BLT tracks the new master at once, but its track is still on its way (the
;; curves are computed on every send, cache hit or not), and one or more of
;; its beats reached QLC+ first - as a jump back in the OLD track, which ran
;; the old flags at the new beat: 5 of 42 handovers on 17-20 Sep drew an
;; extra section, colour or drop for one beat.
;; runde 203: the trigger's beat is the TEMPO MASTER's. It goes out only while
;; the master is also the deck the lights follow; otherwise qlc-on-beat sends
;; the followed deck's beats.
(defn qlc-beat! [beat]
  (let [m      (qlc-tempo-master)
        [p _]  @qlc-sent-track
        f      @qlc-followed]
    ;; ... and the title too (runde 249), as in qlc-tracked-update!
    (when (and p f (== p f) (or (nil? m) (== m p))
               (= (qlc-title f) (second @qlc-sent-track)))
      (qlc-send-position! beat true))))

;; ---------------------------------------------------------------- R561_NO_MASTER: no tempo master
;;
;; Tobias 11-05 ("hullet burde da vaere lukket fra tidligere runde?"): the light
;; stood still for minutes while a deck played. Runde 203/205/360 chose the deck
;; the room hears - but only inside qlc-tracked-update!, and BLT's trigger runs
;; that only for the TEMPO MASTER's status. With no master at all (nobody pressed
;; SYNC or MASTER on a CDJ) the trigger had no player and nothing ran: no track,
;; no beats, no follow. Now this listener stands in, only while there is no CDJ
;; master: the deck the lights follow (while it is on the network), else the
;; lowest deck the room hears - one deck only, so the choice never flips between
;; two. qlc-follow! then hands the lights to the heard deck as always (the 2 s
;; hold, the drop take). With a master everything is as before: the trigger runs
;; it and this listener does nothing. Only read from the decks - nothing goes back.

(defonce qlc-standin-logged (atom nil))   ; the stand-in last written to the log

(defn qlc-cdj-master
  "The tempo master when it is a player 1-6 (a CDJ that sends status), else nil."
  []
  (let [m (qlc-tempo-master)]
    (when (and m (<= 1 m 6)) m)))

(defn qlc-standin-choice
  "Pure: the player that stands in for a missing tempo master, or nil.
   The followed deck while it is on the network, else the lowest heard deck."
  [followed followed-there? heard]
  (cond
    (and followed followed-there?) (long followed)
    (seq heard)                    (long (apply min heard))
    :else                          nil))

(defn qlc-deck-there?
  "Does VirtualCdj have a status for this player now?"
  [p]
  (try (instance? org.deepsymmetry.beatlink.CdjStatus
                  (.getLatestStatusFor (org.deepsymmetry.beatlink.VirtualCdj/getInstance) (int p)))
       (catch Throwable _ false)))

(defn qlc-standin-status!
  "A status update from any device: runs the tracked update for the stand-in
   deck while no CDJ is tempo master. Never throws."
  [status]
  (try
    (when (instance? org.deepsymmetry.beatlink.CdjStatus status)
      (let [^org.deepsymmetry.beatlink.CdjStatus s status
            dev (long (.getDeviceNumber s))]
        (when (<= 1 dev 4)
          (if (qlc-cdj-master)
            (reset! qlc-standin-logged nil)          ; a master again: the trigger has it
            (let [f        @qlc-followed
                  there?   (boolean (and f (qlc-deck-there? f)))
                  stand-in (qlc-standin-choice f there? (when-not there? (qlc-heard-decks)))]
              (when (and stand-in (== dev stand-in))
                (when (not= stand-in @qlc-standin-logged)
                  (reset! qlc-standin-logged stand-in)
                  (try (timbre/info "QLC+: no tempo master - deck" stand-in
                                    "stands in so the lights still follow the room")
                       (catch Throwable _ nil)))
                (qlc-tracked-update! dev (.getBeatNumber s) (.isPlaying s))))))))
    (catch Throwable e
      (try (timbre/warn "QLC+ stand-in for the tempo master failed:" (.getMessage e))
           (catch Throwable _ nil)))))

;; defonce: re-evaluating the shared functions (BLT's Update) must not add a
;; second listener. The reify calls qlc-standin-status! through its var, so a new
;; definition is used. VirtualCdj takes listeners before it is running.
(defonce qlc-standin-listener
  (try
    (let [l (reify org.deepsymmetry.beatlink.DeviceUpdateListener
              (received [_ status] (qlc-standin-status! status)))]
      (.addUpdateListener (org.deepsymmetry.beatlink.VirtualCdj/getInstance) l)
      l)
    (catch Throwable e
      (try (timbre/warn "QLC+ could not listen for players without a tempo master:" (.getMessage e))
           (catch Throwable _ nil))
      nil)))

;; ---------------------------------------------------------------- vaerktoej

(defn qlc-explain! [player]
  (let [wf     (org.deepsymmetry.beatlink.data.WaveformFinder/getInstance)
        bg     (org.deepsymmetry.beatlink.data.BeatGridFinder/getInstance)
        detail (.getLatestDetailFor wf (int player))
        grid   (.getLatestBeatGridFor bg (int player))
        _      (when-not (and detail grid)
                 (throw (ex-info (str "Player " player " has no waveform/beat grid loaded") {})))
        c0     (qlc-curves detail grid)
        ;; med patentgraenser, saa dialogen viser det samme som QLC+ faar
        curves (assoc c0 :patent (qlc-patent-for detail grid c0))
        {:keys [sections phase downbeat ratio target got cues kick-threshold
                grid-trust grid-offset patent patent-found]}
        (qlc-analyse curves grid (qlc-cue-beats grid player))
        n      (count (:energy curves))
        bar    (fn [beat] (inc (quot (- (dec beat) downbeat) 4)))]
    (javax.swing.JOptionPane/showMessageDialog
      nil
      (str "beats: " n "   takter: " (quot n 4)
           "   downbeat: beat " (inc downbeat)
           "   frasefase: takt " (inc phase)
           "\ntaerskel: " (format "%.2f" ratio)
           "   maal: " target "   fik: " got
           "   cues: " cues
           "   patent: " (or patent 0) " af " (or patent-found 0)
           "   kick-taerskel: " (format "%.2f" (double kick-threshold))
           "\ngrid-tillid: " (format "%.0f%%" (* 100.0 (double grid-trust)))
           (if (pos? grid-offset) (str "   (fase rettet " grid-offset " ms)") "")
           (if (qlc-cache-get (qlc-title player)) "   [cached]" "")
           "\nsektioner: " (count sections) "\n"
           (clojure.string/join
             "\n"
             (map #(format "  %-7s takt %3d-%3d  (%2d)  kick=%.2f  e=%.2f  bas=%.2f top=%.2f"
                           (qlc-section-type %)
                           (bar (:from %)) (bar (inc (:to %)))
                           (quot (inc (- (:to %) (:from %))) 4)
                           (double (:kick %)) (double (:energy %))
                           (double (:low %)) (double (:high %)))
                  sections))))))

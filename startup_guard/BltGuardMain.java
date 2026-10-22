package org.qlcplus.rig;

import java.lang.reflect.InvocationTargetException;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardOpenOption;

/** Runs before BLT loads its preferences, network services or Swing windows. */
public final class BltGuardMain {
    // Deliberately retain both for the JVM's whole lifetime, even if main returns
    // while Swing threads remain. Never delete the file: doing so races a new JVM.
    private static FileChannel channel;
    private static FileLock lock;

    private BltGuardMain() { }

    // R492_GUARD_LOG (Tobias 10-22): one line per start beside QLC+'s, in
    // %LOCALAPPDATA%\qlc-rig-update\startup.log - never a reason not to start
    private static void log(String what) {
        try {
            String home = System.getenv("LOCALAPPDATA");
            if (home == null || home.isEmpty()) {
                return;
            }
            Path dir = Paths.get(home, "qlc-rig-update");
            Files.createDirectories(dir);
            String when = new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss").format(new java.util.Date());
            String line = when + "  BLT  " + what + "\r\n";
            Files.write(dir.resolve("startup.log"), line.getBytes(java.nio.charset.StandardCharsets.UTF_8),
                StandardOpenOption.CREATE, StandardOpenOption.APPEND);
        } catch (Throwable ignored) {
            // a log is never a reason not to start (java.base only, whatever runtime BLT ships)
        }
    }

    // R492_GUARD_NOTICE (Tobias 10-22: "ja"): a start the guard stops is no longer silent.
    // QLC_GUARD_QUIET=1 (the guard tests, CI) or no screen: nothing shown. True when shown.
    private static boolean notice(String text) {
        String quiet = System.getenv("QLC_GUARD_QUIET");
        if (quiet != null && !quiet.isEmpty() && !quiet.startsWith("0")) {
            return false;
        }
        try {
            if (java.awt.GraphicsEnvironment.isHeadless()) {
                return false;
            }
            javax.swing.JOptionPane.showMessageDialog(null, text, "Beat Link Trigger",
                javax.swing.JOptionPane.INFORMATION_MESSAGE);
            return true;
        } catch (Throwable ignored) {
            return false;
        }
    }

    public static void main(String[] args) throws Throwable {
        try {
            String shared = System.getenv("ProgramData");
            if (shared == null || shared.isEmpty()) {
                throw new IllegalStateException("ProgramData is missing; cannot acquire the PC lock");
            }
            Path base = Paths.get(shared, "qlc-rig");
            Files.createDirectories(base);
            channel = FileChannel.open(base.resolve("blt-instance.lock"),
                StandardOpenOption.CREATE, StandardOpenOption.WRITE);
            lock = channel.tryLock();
            if (lock == null) {
                channel.close();
                System.err.println("Beat Link Trigger is already running; extra start ignored.");
                log("ignored - Beat Link Trigger is already running");
                // R492_GUARD_NOTICE: the lock is already let go - the notice never blocks a real start
                if (notice("Beat Link Trigger is already running.\n\nThis start was ignored.")) {
                    System.exit(0); // the notice started Swing; nothing else may keep this JVM
                }
                return;
            }
        } catch (Exception failure) {
            System.err.println("Beat Link Trigger startup guard failed: " + failure);
            log("refused - startup guard failed: " + failure);
            notice("Beat Link Trigger could not start: its startup guard failed.\n\n" + failure);
            System.exit(1); // No unguarded fallback.
            return;
        }
        log("started");

        String mainClass = System.getProperty("qlc.blt.main");
        if (!"beat_link_trigger.BeatLinkTrigger".equals(mainClass)) {
            throw new IllegalStateException("Unexpected BLT main class: " + mainClass);
        }
        try {
            Class.forName(mainClass).getMethod("main", String[].class)
                .invoke(null, (Object) args);
        } catch (InvocationTargetException failure) {
            throw failure.getCause();
        }
    }
}

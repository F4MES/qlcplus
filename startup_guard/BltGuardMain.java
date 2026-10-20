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
                return;
            }
        } catch (Exception failure) {
            System.err.println("Beat Link Trigger startup guard failed: " + failure);
            System.exit(1); // No unguarded fallback.
            return;
        }

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

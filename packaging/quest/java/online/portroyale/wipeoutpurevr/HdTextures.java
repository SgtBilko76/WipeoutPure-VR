package online.portroyale.wipeoutpurevr;

import java.io.BufferedInputStream;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintWriter;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.HashMap;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * The HD texture pack, fetched on the first start from its author's GitHub repository (NR74W's WipEout Pure HD
 * texture pack, re-uploaded by 0xf0xx0) and unpacked into textures/UCUS98612 of the app's data folder, where the
 * renderer finds it; the native side switches to it as soon as it is there (enhancements.cpp,
 * vr_hd_textures_poll). Nothing of the pack is in the APK.
 *
 * Runs once per process on a background thread and never stops the game: without a network it tries again at the
 * next start. A pack already installed (or copied there by hand) is left alone, and an empty file "no-hd-textures"
 * in the data folder turns the download off. Progress and problems go to hd-textures.txt in the data folder and to
 * the log (System.out, tag "System.out" in logcat).
 *
 * Plain Java apart from nothing: main() runs it on a PC (java HdTextures DIR) to test the download and the unpacking.
 */
public final class HdTextures implements Runnable {
    static final String COMMIT = "02169319a4c83875cc957d1c59c0017a668934b1";
    static final String[] URLS = {
        "https://codeload.github.com/0xf0xx0/WipEout-Pure-HD-Texture-Pack/zip/" + COMMIT,
        "https://github.com/0xf0xx0/WipEout-Pure-HD-Texture-Pack/archive/" + COMMIT + ".zip",
    };
    static final String DISC_ID = "UCUS98612";
    /** The pack at COMMIT: textures.ini and 1331 ZIM images (+ a few PNGs). */
    static final int MIN_IMAGES = 1331;
    /** The archive's size at COMMIT; GitHub sends no length, so progress and the space check use this. */
    static final long ZIP_BYTES = 454869231L;
    static final int ATTEMPTS = 3;

    private static boolean sStarted;
    private final File mRoot;

    HdTextures(File root) {
        mRoot = root;
    }

    /** Starts the download on its own thread, once per process. */
    public static synchronized void start(File root) {
        if (sStarted || root == null) return;
        sStarted = true;
        Thread thread = new Thread(new HdTextures(root), "hd-textures");
        thread.setDaemon(true);
        thread.setPriority(Thread.MIN_PRIORITY);
        thread.start();
    }

    public static void main(String[] args) {
        new HdTextures(new File(args[0])).run();
    }

    @Override
    public void run() {
        try {
            work();
        } catch (Throwable e) {
            say("failed: " + e);
            status("Download failed (" + e.getMessage() + "). It is tried again at the next start.");
        }
    }

    private void work() throws IOException, InterruptedException {
        Map<String, String> env = readEnv(new File(mRoot, "env.txt"));
        // Test switch: everything goes to textures-test/ instead, beside an installed pack.
        boolean test = "1".equals(env.get("PURE_HD_TEXTURES_TEST"));
        if (!test && new File(mRoot, "no-hd-textures").exists()) {
            say("off (no-hd-textures)");
            return;
        }
        File textures = new File(mRoot, test ? "textures-test" : "textures");
        File target = new File(textures, DISC_ID);
        if (new File(target, "textures.ini").isFile()) {
            say("installed in " + target);
            return;
        }
        long needed = 2 * ZIP_BYTES + (200L << 20);
        if (mRoot.getUsableSpace() < needed) {
            status("Not enough free space for the HD textures: they need " + (needed >> 20) + " MB, "
                    + (mRoot.getUsableSpace() >> 20) + " MB are free.");
            return;
        }
        File zip = new File(mRoot, "hd-textures.zip");
        for (int attempt = 1; ; attempt++) {
            try {
                download(zip);
                break;
            } catch (IOException e) {
                say("download attempt " + attempt + " failed: " + e);
                if (attempt == ATTEMPTS) throw e;
                status("Download interrupted (" + e.getMessage() + "), trying again in 30 s.");
                Thread.sleep(30000);
            }
        }
        try {
            extract(zip, textures, target);
        } finally {
            zip.delete();
        }
        status("HD textures installed (" + target + ").");
    }

    private void download(File zip) throws IOException {
        File part = new File(zip.getPath() + ".part");
        IOException last = null;
        for (String address : URLS) {
            HttpURLConnection connection = null;
            try {
                connection = (HttpURLConnection) new URL(address).openConnection();
                connection.setConnectTimeout(20000);
                connection.setReadTimeout(60000);
                connection.setInstanceFollowRedirects(true);
                connection.setRequestProperty("User-Agent", "WipeoutPureVR");
                int code = connection.getResponseCode();
                if (code != HttpURLConnection.HTTP_OK) throw new IOException("HTTP " + code + " from " + address);
                say("downloading " + address);
                long done = 0;
                int shown = -1;
                byte[] buffer = new byte[1 << 16];
                try (InputStream in = connection.getInputStream(); OutputStream out = new FileOutputStream(part)) {
                    for (int got; (got = in.read(buffer)) > 0; ) {
                        out.write(buffer, 0, got);
                        done += got;
                        int percent = (int) Math.min(99, done * 100 / ZIP_BYTES);
                        if (percent / 5 != shown / 5) {
                            shown = percent;
                            status("Downloading the HD textures: " + percent + " % (" + (done >> 20) + " of about "
                                    + (ZIP_BYTES >> 20) + " MB).");
                        }
                    }
                }
                if (done < ZIP_BYTES / 2) throw new IOException("only " + done + " bytes arrived");
                if (!part.renameTo(zip)) throw new IOException("cannot rename " + part);
                say("downloaded " + done + " bytes");
                return;
            } catch (IOException e) {
                last = e;
                part.delete();
            } finally {
                if (connection != null) connection.disconnect();
            }
        }
        throw last;
    }

    /** Unpacks textures.ini and the images into a staging folder, checks them, then renames it to the target. */
    private void extract(File zip, File textures, File target) throws IOException {
        File staging = new File(textures, DISC_ID + ".incoming");
        deleteTree(staging);
        if (!staging.mkdirs()) throw new IOException("cannot make " + staging);
        status("Unpacking the HD textures...");
        int images = 0;
        boolean ini = false;
        byte[] buffer = new byte[1 << 16];
        // ZipInputStream checks every file's CRC, so a damaged download fails here.
        try (ZipInputStream in = new ZipInputStream(new BufferedInputStream(new FileInputStream(zip), 1 << 16))) {
            for (ZipEntry entry; (entry = in.getNextEntry()) != null; ) {
                if (entry.isDirectory()) continue;
                String name = entry.getName();
                int slash = name.indexOf('/');
                String path = slash >= 0 ? name.substring(slash + 1) : name;  // below WipEout-Pure-...-<commit>/
                String lower = path.toLowerCase();
                boolean wanted = path.equals("textures.ini") || lower.endsWith(".zim") || lower.endsWith(".png");
                if (!wanted || path.isEmpty() || path.startsWith("readme-images/") || path.contains("..")) continue;
                File file = new File(staging, path);
                File parent = file.getParentFile();
                if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot make " + parent);
                try (OutputStream out = new FileOutputStream(file)) {
                    for (int got; (got = in.read(buffer)) > 0; ) out.write(buffer, 0, got);
                }
                if (path.equals("textures.ini")) ini = true;
                else images++;
            }
        }
        if (!ini || images < MIN_IMAGES) {
            deleteTree(staging);
            throw new IOException("the archive held " + images + " images" + (ini ? "" : " and no textures.ini"));
        }
        deleteTree(target);
        if (!staging.renameTo(target)) throw new IOException("cannot rename " + staging + " to " + target);
        say("installed " + images + " images in " + target);
    }

    private static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteTree(child);
        file.delete();
    }

    private static Map<String, String> readEnv(File file) {
        Map<String, String> env = new HashMap<>();
        try (BufferedReader in = new BufferedReader(new FileReader(file))) {
            for (String line; (line = in.readLine()) != null; ) {
                int equals = line.indexOf('=');
                if (line.startsWith("#") || equals <= 0) continue;
                env.put(line.substring(0, equals).trim(), line.substring(equals + 1).trim());
            }
        } catch (IOException e) {
            // no env.txt
        }
        return env;
    }

    private static void say(String line) {
        System.out.println("[hd-textures] " + line);
    }

    /** The player-readable state, in hd-textures.txt, and the log. */
    private void status(String line) {
        say(line);
        try (PrintWriter out = new PrintWriter(new FileWriter(new File(mRoot, "hd-textures.txt")))) {
            out.println(line);
        } catch (IOException e) {
            say("cannot write hd-textures.txt: " + e);
        }
    }
}

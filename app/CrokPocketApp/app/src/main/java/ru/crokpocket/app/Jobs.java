package ru.crokpocket.app;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Environment;
import android.provider.MediaStore;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/** Пакетная обработка выбранных файлов: конвертация и отправка на приставку или сохранение на телефон */
public final class Jobs {
    private Jobs() { }

    /** Интерфейс к окну прогресса */
    public interface Ui {
        /** @return false - пользователь нажал «Отмена» */
        boolean progress(String title, String line, int percent);
    }

    public static Media.VideoOpts videoOpts(SharedPreferences sp) {
        Media.VideoOpts o = new Media.VideoOpts();
        o.fps = sp.getInt("fps", 15);
        o.quality = sp.getInt("quality", 70);
        o.fill = sp.getBoolean("fill", false);
        return o;
    }

    /** Результат конвертации одного файла: готовый файл во временной папке или исходник как есть */
    private static final class Prepared {
        File temp;          // null - отправлять исходный файл
        String name;        // имя на приставке
        long size;
        String mime = "application/octet-stream";
    }

    private static Prepared prepare(Context ctx, Uri uri, String name, Kinds.Kind kind, boolean convert,
                                    Media.VideoOpts vo, Media.Prog prog) throws Exception {
        ContentResolver cr = ctx.getContentResolver();
        Prepared r = new Prepared();
        String base = Kinds.safeName(Kinds.base(name));
        File cache = ctx.getCacheDir();
        if (convert && kind == Kinds.Kind.VIDEO) {
            r.temp = new File(cache, "conv_video.avi");
            Media.convertVideo(ctx, uri, r.temp, vo, prog);
            r.name = base + ".avi";
            r.mime = "video/x-msvideo";
        } else if (convert && kind == Kinds.Kind.IMAGE) {
            r.temp = new File(cache, "conv_image.jpg");
            Media.convertImage(ctx, uri, r.temp);
            r.name = base + ".jpg";
            r.mime = "image/jpeg";
        } else if (convert && kind == Kinds.Kind.AUDIO && !Media.isMp3(cr, uri, name)) {
            r.temp = new File(cache, "conv_audio.wav");
            Media.convertAudio(ctx, uri, r.temp, prog);
            r.name = base + ".wav";
            r.mime = "audio/wav";
        } else {
            r.name = Kinds.safeName(name);
            r.size = Kinds.size(cr, uri);
            if (r.size < 0) {   // размер неизвестен - копируем во временный файл
                r.temp = new File(cache, "conv_copy.bin");
                try (InputStream in = cr.openInputStream(uri); OutputStream os = new FileOutputStream(r.temp)) {
                    copy(in, os);
                }
            }
            String m = cr.getType(uri);
            if (m != null) r.mime = m;
        }
        if (r.temp != null) r.size = r.temp.length();
        return r;
    }

    private static void copy(InputStream in, OutputStream os) throws IOException {
        if (in == null) throw new IOException("Не удалось открыть файл");
        byte[] b = new byte[65536];
        int n;
        while ((n = in.read(b)) > 0) os.write(b, 0, n);
    }

    /**
     * Отправить файлы на приставку.
     * @param fixedDir папка для всех файлов; null - «умная» раскладка по типу (видео, фото, музыка, игры)
     * @param romDirs  для игр: заранее выбранные папки систем (имя файла -> папка), может быть null
     */
    public static String sendToConsole(Context ctx, Console con, List<Uri> uris, String fixedDir, boolean convert,
                                       Map<Uri, String> romDirs, Ui ui) {
        ContentResolver cr = ctx.getContentResolver();
        SharedPreferences sp = ctx.getSharedPreferences("crok", Context.MODE_PRIVATE);
        Media.VideoOpts vo = videoOpts(sp);
        int ok = 0;
        List<String> errors = new ArrayList<>();
        List<String> where = new ArrayList<>();
        for (int i = 0; i < uris.size(); i++) {
            Uri uri = uris.get(i);
            String name = Kinds.displayName(cr, uri);
            String title = "Файл " + (i + 1) + " из " + uris.size() + ": " + name;
            Prepared pr = null;
            try {
                Kinds.Kind kind = Kinds.kind(cr, uri, name);
                String dir = fixedDir;
                if (dir == null) {
                    if (romDirs != null && romDirs.containsKey(uri)) dir = "/roms/" + romDirs.get(uri);
                    else dir = Kinds.smartDir(cr, uri, name, kind);
                }
                pr = prepare(ctx, uri, name, kind, convert, vo, (pct, stage) ->
                        ui.progress(title, "Конвертация (" + stage + "): " + pct + "%", pct * 70 / 100));
                if (!ui.progress(title, "Отправка в " + dir, 70)) throw new Media.CancelledException();
                final long total = pr.size;
                final String d = dir;
                // до 3 попыток: на приставке файл появляется только целиком, обрывок не остаётся
                for (int attempt = 1; ; attempt++) {
                    final String note = attempt > 1 ? " (попытка " + attempt + ")" : "";
                    try (InputStream in = pr.temp != null ? new FileInputStream(pr.temp) : cr.openInputStream(uri)) {
                        if (in == null) throw new IOException("Не удалось открыть файл");
                        con.mkdirs(dir);
                        con.upload(dir, pr.name, in, pr.size, (done, all) ->
                                ui.progress(title, "Отправка в " + d + note + ": " + Kinds.human(done) + " из " + Kinds.human(total),
                                        70 + (int) (30 * done / Math.max(1, all))));
                        break;
                    } catch (IOException e) {
                        if ("Отменено".equals(e.getMessage()) || attempt >= 3) throw e;
                        if (!ui.progress(title, "Сбой передачи, повторяю: " + e.getMessage(), 70)) throw new Media.CancelledException();
                        Thread.sleep(1500);
                        if (!con.isConnected() || Console.isLinkError(e)) {
                            if (!ui.progress(title, "Переподключаюсь к приставке...", 70)) throw new Media.CancelledException();
                            if (!con.ensureConnected(30000))
                                throw new IOException("Связь с приставкой пропала. Проверьте, что на приставке открыт «Wi-Fi», и нажмите «Подключиться».");
                        }
                    }
                }
                ok++;
                if (!where.contains(dir)) where.add(dir);
            } catch (Media.CancelledException e) {
                errors.add("Остановлено пользователем");
                break;
            } catch (Exception e) {
                String m = e.getMessage();
                if ("Отменено".equals(m)) { errors.add("Остановлено пользователем"); break; }
                errors.add(name + ": " + (m == null ? e.getClass().getSimpleName() : m));
            } finally {
                if (pr != null && pr.temp != null) pr.temp.delete();
            }
        }
        StringBuilder sb = new StringBuilder();
        sb.append("Отправлено: ").append(ok).append(" из ").append(uris.size());
        if (!where.isEmpty()) sb.append("\nПапки: ").append(String.join(", ", where));
        for (String e : errors) sb.append("\n• ").append(e);
        return sb.toString();
    }

    /** Сохранить в «Загрузки/CrokPocket» на телефоне (например, чтобы потом скопировать на карту через картридер) */
    public static Uri saveToDownloads(Context ctx, String name, String mime, InputStream in) throws IOException {
        Uri dst = createInDownloads(ctx, "", name, mime);
        try (OutputStream os = ctx.getContentResolver().openOutputStream(dst)) {
            if (os == null) throw new IOException("Не удалось записать файл");
            copy(in, os);
        }
        return dst;
    }

    /** Тип файла по расширению: иначе Android может дописать к имени «.bin» */
    public static String mimeOf(String name) {
        String m = android.webkit.MimeTypeMap.getSingleton().getMimeTypeFromExtension(Kinds.ext(name));
        return m != null ? m : "application/octet-stream";
    }

    /** Новый файл в «Загрузки/CrokPocket/<subdir>» */
    public static Uri createInDownloads(Context ctx, String subdir, String name, String mime) throws IOException {
        ContentValues cv = new ContentValues();
        cv.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
        cv.put(MediaStore.MediaColumns.MIME_TYPE, mime == null ? mimeOf(name) : mime);
        String rel = Environment.DIRECTORY_DOWNLOADS + "/CrokPocket" + (subdir == null || subdir.isEmpty() ? "" : "/" + subdir);
        cv.put(MediaStore.MediaColumns.RELATIVE_PATH, rel);
        Uri dst = ctx.getContentResolver().insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, cv);
        if (dst == null) throw new IOException("Не удалось создать файл в «Загрузках»");
        return dst;
    }

    private static void collect(Console con, String dir, String rel, List<String[]> out, long[] total) throws Exception {
        for (Console.Entry e : con.list(dir)) {
            String p = (dir.endsWith("/") ? dir : dir + "/") + e.name;
            String r = rel.isEmpty() ? e.name : rel + "/" + e.name;
            if (e.dir) collect(con, p, r, out, total);
            else { out.add(new String[]{p, rel, e.name, String.valueOf(e.size)}); total[0] += e.size; }
        }
    }

    /**
     * Скачать с приставки файл или папку целиком (со всеми вложенными) в «Загрузки/CrokPocket».
     * Структура папок сохраняется: /Video/a.avi -> Загрузки/CrokPocket/Video/a.avi
     */
    public static String downloadTree(Context ctx, Console con, String path, boolean isDir, long size, Ui ui) throws Exception {
        List<String[]> files = new ArrayList<>();
        long[] total = {0};
        String top = path.equals("/") ? "SD" : path.substring(path.lastIndexOf('/') + 1);
        if (isDir) {
            ui.progress("Скачивание", "Составляю список файлов...", 0);
            collect(con, path, top, files, total);
        } else {
            files.add(new String[]{path, "", top, String.valueOf(size)});
            total[0] = size;
        }
        if (files.isEmpty()) return "Папка пуста";
        ContentResolver cr = ctx.getContentResolver();
        long base = 0;
        int ok = 0;
        List<String> errors = new ArrayList<>();
        for (int i = 0; i < files.size(); i++) {
            String[] f = files.get(i);
            final long b0 = base, all = Math.max(1, total[0]);
            final String title = "Файл " + (i + 1) + " из " + files.size() + ": " + f[2];
            Uri dst = null;
            try {
                dst = createInDownloads(ctx, f[1], f[2], null);
                try (OutputStream os = cr.openOutputStream(dst)) {
                    if (os == null) throw new IOException("Не удалось записать файл");
                    con.download(f[0], os, (done, t) -> ui.progress(title,
                            Kinds.human(b0 + done) + " из " + Kinds.human(total[0]), (int) (100 * (b0 + done) / all)));
                }
                ok++;
            } catch (Exception e) {
                if (dst != null) try { cr.delete(dst, null, null); } catch (Exception ignored) { }
                String m = e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage();
                if (m.equals("Отменено")) { errors.add("Остановлено пользователем"); break; }
                errors.add(f[2] + ": " + m);
            }
            base += Long.parseLong(f[3]);
        }
        StringBuilder sb = new StringBuilder("Скачано в «Загрузки/CrokPocket" + (isDir ? "/" + top : "") + "»: " + ok + " из " + files.size());
        for (int i = 0; i < errors.size() && i < 10; i++) sb.append("\n• ").append(errors.get(i));
        if (errors.size() > 10) sb.append("\n... и ещё ").append(errors.size() - 10);
        return sb.toString();
    }

    public static String convertToPhone(Context ctx, List<Uri> uris, Ui ui) {
        ContentResolver cr = ctx.getContentResolver();
        SharedPreferences sp = ctx.getSharedPreferences("crok", Context.MODE_PRIVATE);
        Media.VideoOpts vo = videoOpts(sp);
        int ok = 0, skipped = 0;
        List<String> errors = new ArrayList<>();
        for (int i = 0; i < uris.size(); i++) {
            Uri uri = uris.get(i);
            String name = Kinds.displayName(cr, uri);
            String title = "Файл " + (i + 1) + " из " + uris.size() + ": " + name;
            Prepared pr = null;
            try {
                Kinds.Kind kind = Kinds.kind(cr, uri, name);
                if (kind == Kinds.Kind.ROM || kind == Kinds.Kind.OTHER) { skipped++; continue; }
                pr = prepare(ctx, uri, name, kind, true, vo, (pct, stage) ->
                        ui.progress(title, "Конвертация (" + stage + "): " + pct + "%", pct * 90 / 100));
                if (pr.temp == null) { skipped++; continue; }   // MP3 и так подходит
                ui.progress(title, "Сохранение в «Загрузки/CrokPocket»", 95);
                try (InputStream in = new FileInputStream(pr.temp)) {
                    saveToDownloads(ctx, pr.name, pr.mime, in);
                }
                ok++;
            } catch (Media.CancelledException e) {
                errors.add("Остановлено пользователем");
                break;
            } catch (Exception e) {
                errors.add(name + ": " + (e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage()));
            } finally {
                if (pr != null && pr.temp != null) pr.temp.delete();
            }
        }
        StringBuilder sb = new StringBuilder("Сохранено в «Загрузки/CrokPocket»: " + ok);
        if (skipped > 0) sb.append("\nНе требуют конвертации: ").append(skipped);
        for (String e : errors) sb.append("\n• ").append(e);
        return sb.toString();
    }
}

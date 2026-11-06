package ru.crokpocket.app;

import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;

import java.io.InputStream;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** Что это за файл и куда его класть на SD-карте приставки */
public final class Kinds {
    private Kinds() { }

    public enum Kind { VIDEO, IMAGE, AUDIO, ROM, OTHER }

    public static final String DIR_VIDEO = "/Video";
    public static final String DIR_PICTURES = "/Pictures";
    public static final String DIR_MUSIC = "/Music";
    public static final String DIR_FILES = "/Files";

    /** Расширение ROM -> папка Retro-Go (/roms/<папка>) */
    public static final Map<String, String> ROM_DIRS = new LinkedHashMap<>();
    /** Папка Retro-Go -> название системы для меню */
    public static final Map<String, String> SYSTEMS = new LinkedHashMap<>();

    static {
        SYSTEMS.put("nes", "NES / Денди");
        SYSTEMS.put("snes", "Super Nintendo");
        SYSTEMS.put("gb", "Game Boy");
        SYSTEMS.put("gbc", "Game Boy Color");
        SYSTEMS.put("md", "Sega Mega Drive");
        SYSTEMS.put("sms", "Sega Master System");
        SYSTEMS.put("gg", "Sega Game Gear");
        SYSTEMS.put("col", "ColecoVision");
        SYSTEMS.put("pce", "PC Engine");
        SYSTEMS.put("lnx", "Atari Lynx");
        SYSTEMS.put("gw", "Game & Watch");
        SYSTEMS.put("doom", "DOOM (WAD)");

        ROM_DIRS.put("nes", "nes"); ROM_DIRS.put("fds", "nes");
        ROM_DIRS.put("smc", "snes"); ROM_DIRS.put("sfc", "snes");
        ROM_DIRS.put("gb", "gb");
        ROM_DIRS.put("gbc", "gbc");
        ROM_DIRS.put("md", "md"); ROM_DIRS.put("gen", "md"); ROM_DIRS.put("smd", "md");
        ROM_DIRS.put("sms", "sms");
        ROM_DIRS.put("gg", "gg");
        ROM_DIRS.put("col", "col");
        ROM_DIRS.put("pce", "pce");
        ROM_DIRS.put("lnx", "lnx");
        ROM_DIRS.put("gw", "gw");
        ROM_DIRS.put("wad", "doom");
    }

    public static String ext(String name) {
        int d = name.lastIndexOf('.');
        return d < 0 ? "" : name.substring(d + 1).toLowerCase(Locale.ROOT);
    }

    public static String base(String name) {
        int d = name.lastIndexOf('.');
        return d <= 0 ? name : name.substring(0, d);
    }

    /** Имя, которое примет FAT на SD-карте: без запрещённых символов и не слишком длинное */
    public static String safeName(String name) {
        String s = name.replaceAll("[\\\\/:*?\"<>|\\p{Cntrl}]", "_").trim();
        while (s.startsWith(".")) s = s.substring(1);
        if (s.isEmpty()) s = "file";
        String e = ext(s);
        String b = base(s);
        if (b.length() > 60) b = b.substring(0, 60).trim();
        return e.isEmpty() ? b : b + "." + e;
    }

    public static String displayName(ContentResolver cr, Uri uri) {
        String n = null;
        try (Cursor c = cr.query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) n = c.getString(0);
        } catch (Exception ignored) { }
        if (n == null) {
            n = uri.getLastPathSegment();
            if (n == null) n = "file";
            int sl = n.lastIndexOf('/');
            if (sl >= 0) n = n.substring(sl + 1);
        }
        return n;
    }

    public static long size(ContentResolver cr, Uri uri) {
        try (Cursor c = cr.query(uri, new String[]{OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getLong(0);
        } catch (Exception ignored) { }
        return -1;
    }

    public static Kind kind(ContentResolver cr, Uri uri, String name) {
        String e = ext(name);
        if (ROM_DIRS.containsKey(e)) return Kind.ROM;
        if (e.equals("zip") && romDirOfZip(cr, uri) != null) return Kind.ROM;
        String mime = null;
        try { mime = cr.getType(uri); } catch (Exception ignored) { }
        if (mime == null) mime = "";
        if (mime.startsWith("video/") || e.matches("mp4|m4v|mkv|webm|3gp|mov|avi|ts")) return Kind.VIDEO;
        if (mime.startsWith("image/") || e.matches("jpg|jpeg|png|webp|heic|heif|bmp|gif")) return Kind.IMAGE;
        if (mime.startsWith("audio/") || e.matches("mp3|m4a|aac|ogg|opus|flac|wav|amr")) return Kind.AUDIO;
        return Kind.OTHER;
    }

    /** ZIP с одной игрой: определяем систему по расширению первого файла внутри */
    public static String romDirOfZip(ContentResolver cr, Uri uri) {
        try (InputStream in = cr.openInputStream(uri); ZipInputStream z = new ZipInputStream(in)) {
            ZipEntry en;
            int n = 0;
            while ((en = z.getNextEntry()) != null && n++ < 20) {
                if (en.isDirectory()) continue;
                String d = ROM_DIRS.get(ext(en.getName()));
                if (d != null) return d;
            }
        } catch (Exception ignored) { }
        return null;
    }

    public static String romDir(ContentResolver cr, Uri uri, String name) {
        String e = ext(name);
        if (e.equals("zip")) return romDirOfZip(cr, uri);
        return ROM_DIRS.get(e);
    }

    /** Папка «по типу» для умной загрузки */
    public static String smartDir(ContentResolver cr, Uri uri, String name, Kind k) {
        switch (k) {
            case VIDEO: return DIR_VIDEO;
            case IMAGE: return DIR_PICTURES;
            case AUDIO: return DIR_MUSIC;
            case ROM: {
                String d = romDir(cr, uri, name);
                return d == null ? DIR_FILES : "/roms/" + d;
            }
            default: return DIR_FILES;
        }
    }

    public static String human(long b) {
        if (b < 0) return "?";
        if (b < 1024) return b + " Б";
        if (b < 1024L * 1024) return String.format(Locale.ROOT, "%.1f КБ", b / 1024.0);
        if (b < 1024L * 1024 * 1024) return String.format(Locale.ROOT, "%.1f МБ", b / 1048576.0);
        return String.format(Locale.ROOT, "%.2f ГБ", b / 1073741824.0);
    }
}

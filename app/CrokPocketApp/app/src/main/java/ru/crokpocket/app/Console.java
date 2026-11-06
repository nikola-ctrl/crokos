package ru.crokpocket.app;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.wifi.WifiNetworkSpecifier;
import android.os.Handler;
import android.os.Looper;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * Связь с приставкой: она раздаёт открытую сеть «CrokPocket» (CrokOS -> «Wi-Fi»), адрес 192.168.4.1.
 * В той сети нет интернета, поэтому Android охотно отправляет запросы через мобильную сеть -
 * все запросы идут строго через найденную Wi-Fi сеть (Network.openConnection).
 */
public class Console {
    public static final String SSID = "CrokPocket";
    public static final String BASE = "http://192.168.4.1";

    public interface Listener { void onState(boolean connected, String message); }

    public static class Entry {
        public String name;
        public boolean dir;
        public long size;
    }

    public static class Info {
        public String app = "", ver = "";
        public long total, free;
    }

    public interface Progress {
        /** @return false - отменить */
        boolean onProgress(long done, long total);
    }

    private final Context ctx;
    private final ConnectivityManager cm;
    private final Handler main = new Handler(Looper.getMainLooper());
    private volatile Network network;
    private ConnectivityManager.NetworkCallback callback;
    private Listener listener;

    public Console(Context ctx) {
        this.ctx = ctx.getApplicationContext();
        this.cm = (ConnectivityManager) ctx.getSystemService(Context.CONNECTIVITY_SERVICE);
    }

    public void setListener(Listener l) { listener = l; }

    public boolean isConnected() { return network != null; }

    private void state(boolean ok, String msg) {
        main.post(() -> { if (listener != null) listener.onState(ok, msg); });
    }

    /** Подключение: сначала ищем уже подключённую сеть приставки, иначе просим систему подключиться к «CrokPocket». */
    public void connect() {
        new Thread(() -> {
            // 1. Телефон уже в сети приставки (подключились вручную)?
            for (Network n : cm.getAllNetworks()) {
                NetworkCapabilities c = cm.getNetworkCapabilities(n);
                if (c == null || !c.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) continue;
                if (probe(n)) { network = n; state(true, "Подключено к приставке"); return; }
            }
            main.post(this::requestSpecifier);
        }).start();
    }

    private void requestSpecifier() {
        disconnect();
        state(false, "Подключаюсь к сети «" + SSID + "»... Подтвердите подключение в окне Android.");
        WifiNetworkSpecifier spec = new WifiNetworkSpecifier.Builder().setSsid(SSID).build();
        NetworkRequest req = new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .setNetworkSpecifier(spec)
                .build();
        callback = new ConnectivityManager.NetworkCallback() {
            @Override public void onAvailable(Network n) {
                new Thread(() -> {
                    for (int i = 0; i < 5; i++) {
                        if (probe(n)) { network = n; state(true, "Подключено к приставке"); return; }
                        sleep(800);
                    }
                    network = n;
                    state(true, "Сеть подключена, но приставка не отвечает. На приставке должно быть открыто «Wi-Fi».");
                }).start();
            }
            @Override public void onUnavailable() {
                network = null;
                state(false, "Сеть «" + SSID + "» не найдена. Откройте на приставке пункт «Wi-Fi» и попробуйте снова.");
            }
            @Override public void onLost(Network n) {
                if (n.equals(network)) { network = null; state(false, "Связь с приставкой потеряна"); }
            }
        };
        try {
            cm.requestNetwork(req, callback, 60000);
        } catch (SecurityException e) {
            state(false, "Нет разрешения на управление Wi-Fi: " + e.getMessage());
        }
    }

    public void disconnect() {
        if (callback != null) {
            try { cm.unregisterNetworkCallback(callback); } catch (Exception ignored) { }
            callback = null;
        }
        network = null;
    }

    private boolean probe(Network n) {
        try {
            HttpURLConnection c = (HttpURLConnection) n.openConnection(new URL(BASE + "/list?dir=%2F"));
            c.setConnectTimeout(2500);
            c.setReadTimeout(4000);
            int code = c.getResponseCode();
            c.disconnect();
            return code == 200;
        } catch (IOException e) {
            return false;
        }
    }

    /**
     * Сеть приставки пропала (приставка перезагрузилась, вышли из «Wi-Fi», Android отключил сеть
     * без интернета, пока приложение было в фоне), а старый Network остался: Android отвечает
     * «Binding socket to network ... failed: EPERM». Такую сеть надо забыть и подключиться заново.
     */
    public static boolean isLinkError(Throwable e) {
        String m = String.valueOf(e.getMessage());
        return m.contains("EPERM") || m.contains("Binding socket") || m.contains("ENONET")
                || m.contains("ENETUNREACH") || m.contains("Network is unreachable");
    }

    private IOException lost(IOException e) {
        if (!isLinkError(e)) return e;
        disconnect();
        state(false, "Связь с приставкой пропала. Нажмите «Подключиться».");
        return new IOException("Связь с приставкой пропала (" + e.getMessage() + ")", e);
    }

    /**
     * Дождаться связи с приставкой, при необходимости переподключившись. Вызывать не из UI-потока.
     * @return true - приставка отвечает
     */
    public boolean ensureConnected(long timeoutMs) {
        Network n = network;
        if (n != null && probe(n)) return true;
        disconnect();
        connect();
        long end = System.currentTimeMillis() + timeoutMs;
        while (System.currentTimeMillis() < end) {
            sleep(500);
            n = network;
            if (n != null && probe(n)) return true;
        }
        return false;
    }

    private static void sleep(long ms) { try { Thread.sleep(ms); } catch (InterruptedException ignored) { } }

    private HttpURLConnection open(String pathAndQuery, int readTimeout) throws IOException {
        Network n = network;
        if (n == null) throw new IOException("Нет связи с приставкой. Нажмите «Подключиться».");
        HttpURLConnection c = (HttpURLConnection) n.openConnection(new URL(BASE + pathAndQuery));
        c.setConnectTimeout(5000);
        c.setReadTimeout(readTimeout);
        return c;
    }

    public static String enc(String s) {
        try { return URLEncoder.encode(s, "UTF-8").replace("+", "%20"); }
        catch (Exception e) { return s; }
    }

    private static String readAll(InputStream in) throws IOException {
        ByteArrayOutputStream bo = new ByteArrayOutputStream();
        byte[] b = new byte[8192];
        int r;
        while ((r = in.read(b)) > 0) bo.write(b, 0, r);
        return bo.toString("UTF-8");
    }

    private String get(String pq) throws IOException {
        HttpURLConnection c = open(pq, 20000);
        try {
            int code = c.getResponseCode();
            if (code == 403) throw new IOException("Приставка запретила это действие (служебный файл)");
            if (code == 409) throw new IOException("Файл с таким именем уже есть");
            if (code != 200) throw new IOException("Ошибка приставки (" + code + ")");
            return readAll(c.getInputStream());
        } catch (IOException e) {
            throw lost(e);
        } finally { c.disconnect(); }
    }

    public List<Entry> list(String dir) throws Exception {
        JSONArray a = new JSONArray(get("/list?dir=" + enc(dir)));
        List<Entry> out = new ArrayList<>();
        for (int i = 0; i < a.length(); i++) {
            JSONObject o = a.getJSONObject(i);
            Entry e = new Entry();
            e.name = o.optString("n");
            e.dir = o.optInt("d") == 1;
            e.size = o.optLong("s");
            out.add(e);
        }
        out.sort((x, y) -> x.dir != y.dir ? (x.dir ? -1 : 1) : x.name.compareToIgnoreCase(y.name));
        return out;
    }

    /** /info есть в CrokOS с октября 2026; на старой прошивке вернёт null */
    public Info info() {
        try {
            JSONObject o = new JSONObject(get("/info"));
            Info i = new Info();
            i.app = o.optString("app");
            i.ver = o.optString("ver");
            i.total = o.optLong("total");
            i.free = o.optLong("free");
            return i;
        } catch (Exception e) {
            return null;
        }
    }

    public void delete(String path) throws IOException { get("/del?path=" + enc(path)); }

    public void mkdir(String path) throws IOException {
        try { get("/mkdir?path=" + enc(path)); }
        catch (IOException e) { if (isLinkError(e)) throw e; /* иначе папка уже есть */ }
    }

    /** Создать все папки пути, например /roms/nes */
    public void mkdirs(String dir) throws IOException {
        String[] parts = dir.split("/");
        String cur = "";
        for (String p : parts) {
            if (p.isEmpty()) continue;
            cur += "/" + p;
            mkdir(cur);
        }
    }

    public void rename(String from, String to) throws IOException {
        get("/rename?from=" + enc(from) + "&to=" + enc(to));
    }

    public void download(String path, OutputStream out, Progress p) throws IOException {
        HttpURLConnection c = open("/dl?path=" + enc(path), 30000);
        try {
            int code = c.getResponseCode();
            if (code == 403) throw new IOException("Этот файл скачать нельзя (в нём пароли и ключи)");
            if (code != 200) throw new IOException("Ошибка приставки (" + code + ")");
            long total = c.getContentLengthLong();
            InputStream in = c.getInputStream();
            byte[] b = new byte[16384];
            long done = 0;
            int r;
            while ((r = in.read(b)) > 0) {
                out.write(b, 0, r);
                done += r;
                if (p != null && !p.onProgress(done, total)) throw new IOException("Отменено");
            }
            if (total > 0 && done != total) throw new IOException("Скачано не полностью: " + done + " из " + total + " байт");
        } catch (IOException e) {
            throw lost(e);
        } finally { c.disconnect(); }
    }

    /** Загрузка файла (multipart/form-data, как из браузера), потоково - без чтения файла в память */
    public void upload(String dir, String name, InputStream data, long size, Progress p) throws IOException {
        String boundary = "----CrokPocket" + System.currentTimeMillis();
        String safeName = name.replace("\"", "'");
        byte[] head = ("--" + boundary + "\r\n"
                + "Content-Disposition: form-data; name=\"file\"; filename=\"" + safeName + "\"\r\n"
                + "Content-Type: application/octet-stream\r\n\r\n").getBytes(StandardCharsets.UTF_8);
        byte[] tail = ("\r\n--" + boundary + "--\r\n").getBytes(StandardCharsets.UTF_8);
        HttpURLConnection c = open("/upload?dir=" + enc(dir), 60000);
        try {
            c.setDoOutput(true);
            c.setRequestMethod("POST");
            c.setRequestProperty("Content-Type", "multipart/form-data; boundary=" + boundary);
            c.setFixedLengthStreamingMode(head.length + size + tail.length);
            OutputStream out = c.getOutputStream();
            out.write(head);
            byte[] b = new byte[8192];
            long done = 0;
            int r;
            while (done < size && (r = data.read(b, 0, (int) Math.min(b.length, size - done))) > 0) {
                out.write(b, 0, r);
                done += r;
                if (p != null && !p.onProgress(done, size)) { c.disconnect(); throw new IOException("Отменено"); }
            }
            if (done != size) throw new IOException("Файл прочитан не полностью");
            out.write(tail);
            out.flush();
            int code = c.getResponseCode();
            if (code != 200) {
                String why = "";
                try { InputStream es = c.getErrorStream(); if (es != null) why = readAll(es).trim(); } catch (Exception ignored) { }
                throw new IOException("Приставка не приняла файл" + (why.isEmpty() ? " (" + code + ")" : ": " + why));
            }
        } catch (IOException e) {
            throw lost(e);
        } finally { c.disconnect(); }
        // проверка: файл на карте должен быть ровно того же размера
        long onCard = -1;
        try {
            for (Entry e : list(dir)) if (!e.dir && e.name.equals(name)) { onCard = e.size; break; }
        } catch (Exception ignored) { }
        if (onCard >= 0 && onCard != size)
            throw new IOException("На карту записано " + onCard + " из " + size + " байт");
    }
}

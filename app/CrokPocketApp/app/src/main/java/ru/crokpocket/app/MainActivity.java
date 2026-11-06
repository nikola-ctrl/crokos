package ru.crokpocket.app;

import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.graphics.Color;
import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.ArrayAdapter;
import android.widget.BaseAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class MainActivity extends Activity {
    private static final int REQ_UPLOAD_HERE = 1, REQ_SEND_SMART = 2, REQ_SAVE_PHONE = 3, REQ_GAMES = 4, REQ_PERM = 10;

    private Console console;
    private SharedPreferences sp;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService bg = Executors.newSingleThreadExecutor();

    // цвета веб-страницы приставки (webfiles.h)
    static final int C_BG = 0xFF111111, C_PANEL = 0xFF222222, C_BTN = 0xFF333333, C_BTN_DOWN = 0xFF444444,
            C_BORDER = 0xFF555555, C_LINE = 0xFF2A2A2A, C_TEXT = 0xFFDDDDDD, C_DIM = 0xFF888888,
            C_ACCENT = 0xFFFF9900, C_DIR = 0xFF99CCFF, C_OK = 0xFF88CC88;

    private TextView statusText;
    private Button connectBtn;
    private FrameLayout content;
    private final List<TextView> tabButtons = new ArrayList<>();
    private View[] pages;

    // «Файлы»
    private String curDir = "/";
    private final List<Console.Entry> entries = new ArrayList<>();
    private BaseAdapter entryAdapter;
    private TextView pathText, freeText;
    private Switch convertSwitch;

    // файлы, пришедшие через «Поделиться», ждут подключения
    private Runnable pendingAction;

    // ------------------------------------------------------------------ каркас

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        sp = getSharedPreferences("crok", MODE_PRIVATE);
        console = new Console(this);
        console.setListener(this::onConsoleState);

        LinearLayout root = vbox();
        root.setPadding(0, 0, 0, 0);

        // шапка как на сайте: #222 и оранжевая полоса снизу
        LinearLayout header = vbox();
        header.setBackgroundColor(C_PANEL);
        LinearLayout bar = hbox();
        bar.setPadding(dp(12), dp(8), dp(8), dp(6));
        bar.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout titles = vbox();
        TextView title = text("CrokPocket", 18);
        title.setTextColor(C_TEXT);
        titles.addView(title);
        statusText = text("Не подключено к приставке", 13);
        statusText.setTextColor(C_DIM);
        titles.addView(statusText);
        bar.addView(titles, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        connectBtn = button("Подключиться", v -> connect());
        bar.addView(connectBtn);
        header.addView(bar);

        LinearLayout tabs = hbox();
        tabs.setPadding(dp(4), 0, dp(4), dp(4));
        String[] names = {"Файлы", "Конвертер", "Игры", "Настройки"};
        for (int i = 0; i < names.length; i++) {
            final int idx = i;
            TextView t = tabView(names[i], v -> showPage(idx));
            tabButtons.add(t);
            tabs.addView(t, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        }
        header.addView(tabs);
        View stripe = new View(this);
        stripe.setBackgroundColor(C_ACCENT);
        header.addView(stripe, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(2)));
        root.addView(header);

        content = new FrameLayout(this);
        root.addView(content, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        pages = new View[]{buildFilesPage(), buildConvertPage(), buildGamesPage(), buildSettingsPage()};
        for (View p : pages) { p.setVisibility(View.GONE); content.addView(p); }
        setContentView(root);
        showPage(0);
        handleShare(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        handleShare(intent);
    }

    @Override
    protected void onDestroy() {
        console.disconnect();
        bg.shutdownNow();
        super.onDestroy();
    }

    private void showPage(int idx) {
        for (int i = 0; i < pages.length; i++) {
            pages[i].setVisibility(i == idx ? View.VISIBLE : View.GONE);
            TextView t = tabButtons.get(i);
            t.setTypeface(null, i == idx ? Typeface.BOLD : Typeface.NORMAL);
            t.setAlpha(i == idx ? 1f : 0.6f);
            t.setTextColor(i == idx ? C_ACCENT : C_TEXT);
            if (i == idx) {
                GradientDrawable bg = new GradientDrawable();
                bg.setColor(C_BTN);
                bg.setCornerRadius(dp(6));
                t.setBackground(bg);
            } else {
                t.setBackgroundColor(Color.TRANSPARENT);
            }
        }
    }

    // ------------------------------------------------------------------ подключение

    private void connect() {
        List<String> need = new ArrayList<>();
        if (Build.VERSION.SDK_INT >= 33) {
            if (checkSelfPermission(Manifest.permission.NEARBY_WIFI_DEVICES) != PackageManager.PERMISSION_GRANTED)
                need.add(Manifest.permission.NEARBY_WIFI_DEVICES);
        } else if (checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED) {
            need.add(Manifest.permission.ACCESS_FINE_LOCATION);
        }
        if (!need.isEmpty()) { requestPermissions(need.toArray(new String[0]), REQ_PERM); return; }
        statusText.setText("Ищу приставку...");
        console.connect();
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] res) {
        super.onRequestPermissionsResult(code, perms, res);
        if (code == REQ_PERM) { statusText.setText("Ищу приставку..."); console.connect(); }
    }

    private void onConsoleState(boolean ok, String msg) {
        statusText.setText(msg);
        statusText.setTextColor(ok ? C_OK : C_DIM);
        connectBtn.setText(ok ? "Обновить" : "Подключиться");
        if (ok) {
            loadDir(curDir);
            if (pendingAction != null) { Runnable r = pendingAction; pendingAction = null; r.run(); }
        }
    }

    private boolean needConnection() {
        if (console.isConnected()) return false;
        new AlertDialog.Builder(this)
                .setTitle("Нет связи с приставкой")
                .setMessage("1. На приставке откройте пункт «Wi-Fi» (загрузка файлов).\n2. Нажмите «Подключиться» и подтвердите подключение к сети «CrokPocket».")
                .setPositiveButton("Подключиться", (d, w) -> connect())
                .setNegativeButton("Отмена", null)
                .show();
        return true;
    }

    // ------------------------------------------------------------------ «Поделиться»

    private void handleShare(Intent in) {
        if (in == null) return;
        List<Uri> uris = new ArrayList<>();
        if (Intent.ACTION_SEND.equals(in.getAction())) {
            Uri u = in.getParcelableExtra(Intent.EXTRA_STREAM);
            if (u != null) uris.add(u);
        } else if (Intent.ACTION_SEND_MULTIPLE.equals(in.getAction())) {
            ArrayList<Uri> l = in.getParcelableArrayListExtra(Intent.EXTRA_STREAM);
            if (l != null) uris.addAll(l);
        }
        if (uris.isEmpty()) return;
        setIntent(new Intent());
        new AlertDialog.Builder(this)
                .setTitle("Файлов: " + uris.size())
                .setItems(new String[]{"Отправить на приставку...", "Конвертировать и сохранить на телефон"}, (d, w) -> {
                    if (w == 0) askShareOptions(uris); else saveToPhone(uris);
                })
                .setNegativeButton("Отмена", null)
                .show();
    }

    /** Выполнить, когда есть связь с приставкой (при необходимости сначала подключиться) */
    private void whenConnected(Runnable r) {
        if (console.isConnected()) { r.run(); return; }
        pendingAction = r;
        toast("Подключаюсь к приставке...");
        connect();
    }

    /** Куда класть и конвертировать ли - спрашиваем при «Поделиться» */
    private void askShareOptions(List<Uri> uris) {
        LinearLayout box = vbox();
        box.setPadding(dp(20), dp(8), dp(20), 0);
        android.widget.CheckBox conv = new android.widget.CheckBox(this);
        conv.setText("Конвертировать для приставки (видео → AVI, фото → JPEG, музыка → WAV)");
        conv.setChecked(sp.getBoolean("convertShare", true));
        box.addView(conv);
        box.addView(label("Куда положить:"));
        android.widget.RadioGroup rg = new android.widget.RadioGroup(this);
        String last = sp.getString("lastShareDir", "");
        String[] labels = last.isEmpty()
                ? new String[]{"По типу: /Video, /Pictures, /Music, /roms/…", "Выбрать папку на приставке…"}
                : new String[]{"По типу: /Video, /Pictures, /Music, /roms/…", "В прошлую папку: " + last, "Выбрать папку на приставке…"};
        for (int i = 0; i < labels.length; i++) {
            android.widget.RadioButton rb = new android.widget.RadioButton(this);
            rb.setText(labels[i]);
            rb.setId(1000 + i);
            rg.addView(rb);
        }
        int mode = Math.min(sp.getInt("shareMode", 0), labels.length - 1);
        rg.check(1000 + mode);
        box.addView(rg);
        new AlertDialog.Builder(this)
                .setTitle("Отправить на приставку: " + uris.size())
                .setView(scroll(box))
                .setPositiveButton("Дальше", (d, w) -> {
                    boolean c = conv.isChecked();
                    int m = rg.getCheckedRadioButtonId() - 1000;
                    sp.edit().putBoolean("convertShare", c).putInt("shareMode", m).apply();
                    boolean pickMode = m == labels.length - 1 && m > 0;
                    if (m == 0) whenConnected(() -> runSend(uris, null, c, null));
                    else if (!pickMode) whenConnected(() -> runSend(uris, last, c, null));
                    else whenConnected(() -> pickFolder("/", dir -> {
                        sp.edit().putString("lastShareDir", dir).apply();
                        runSend(uris, dir, c, null);
                    }));
                })
                .setNegativeButton("Отмена", null)
                .show();
    }

    private interface DirCb { void on(String dir); }

    /** Выбор папки на SD-карте приставки */
    private void pickFolder(String dir, DirCb cb) {
        bg.execute(() -> {
            List<String> sub = new ArrayList<>();
            String err = null;
            try { for (Console.Entry e : console.list(dir)) if (e.dir) sub.add(e.name); }
            catch (Exception e) { err = e.getMessage(); }
            final String error = err;
            main.post(() -> {
                if (error != null) { toast("Не удалось открыть " + dir + ": " + error); return; }
                List<String> items = new ArrayList<>();
                items.add("✔ Положить сюда");
                if (!dir.equals("/")) items.add("↑ Наверх");
                items.add("+ Новая папка");
                for (String s : sub) items.add("📁 " + s);
                new AlertDialog.Builder(this)
                        .setTitle("Папка: " + dir)
                        .setItems(items.toArray(new String[0]), (d, w) -> {
                            String it = items.get(w);
                            if (it.startsWith("✔")) cb.on(dir);
                            else if (it.startsWith("↑")) {
                                int s = dir.lastIndexOf('/');
                                pickFolder(s <= 0 ? "/" : dir.substring(0, s), cb);
                            } else if (it.startsWith("+")) {
                                EditText ed = new EditText(this);
                                ed.setSingleLine();
                                new AlertDialog.Builder(this).setTitle("Новая папка в " + dir).setView(padded(ed))
                                        .setPositiveButton("Создать", (d2, w2) -> {
                                            String n = Kinds.safeName(ed.getText().toString());
                                            String p = join(dir, n);
                                            bg.execute(() -> {
                                                try { console.mkdir(p); main.post(() -> pickFolder(p, cb)); }
                                                catch (Exception e) { main.post(() -> toast(e.getMessage())); }
                                            });
                                        })
                                        .setNegativeButton("Отмена", (d2, w2) -> pickFolder(dir, cb)).show();
                            } else pickFolder(join(dir, it.substring(it.indexOf(' ') + 1).trim()), cb);
                        })
                        .setNegativeButton("Отмена", null)
                        .show();
            });
        });
    }

    // ------------------------------------------------------------------ выбор файлов

    private void pick(int req) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        startActivityForResult(i, req);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null) return;
        List<Uri> uris = new ArrayList<>();
        ClipData cd = data.getClipData();
        if (cd != null) for (int i = 0; i < cd.getItemCount(); i++) uris.add(cd.getItemAt(i).getUri());
        else if (data.getData() != null) uris.add(data.getData());
        if (uris.isEmpty()) return;
        switch (req) {
            case REQ_UPLOAD_HERE: runSend(uris, curDir, convertSwitch.isChecked(), null); break;
            case REQ_SEND_SMART: askShareOptions(uris); break;
            case REQ_SAVE_PHONE: saveToPhone(uris); break;
            case REQ_GAMES: sendGames(uris); break;
        }
    }

    // ------------------------------------------------------------------ фоновые задачи с окном прогресса

    private interface Work { String run(Jobs.Ui ui) throws Exception; }

    private void runWithProgress(String title, Work w, Runnable after) {
        LinearLayout box = vbox();
        box.setPadding(dp(20), dp(12), dp(20), dp(4));
        TextView t1 = text("", 14);
        TextView t2 = text("Подготовка...", 13);
        ProgressBar pb = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        pb.setMax(100);
        box.addView(t1);
        box.addView(pb);
        box.addView(t2);
        final boolean[] cancel = {false};
        AlertDialog dlg = new AlertDialog.Builder(this)
                .setTitle(title)
                .setView(box)
                .setCancelable(false)
                .setNegativeButton("Отмена", null)
                .create();
        dlg.setOnShowListener(d -> dlg.getButton(AlertDialog.BUTTON_NEGATIVE).setOnClickListener(v -> {
            cancel[0] = true;
            t2.setText("Останавливаю...");
        }));
        dlg.show();
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        final long[] lastUi = {0};
        Jobs.Ui ui = (head, line, pct) -> {
            long now = System.currentTimeMillis();
            if (now - lastUi[0] > 150 || pct >= 100) {
                lastUi[0] = now;
                main.post(() -> { t1.setText(head); t2.setText(line); pb.setProgress(pct); });
            }
            return !cancel[0];
        };
        bg.execute(() -> {
            String result;
            try { result = w.run(ui); }
            catch (Exception e) { result = "Ошибка: " + (e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage()); }
            final String r = result;
            main.post(() -> {
                getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                dlg.dismiss();
                if (r != null && !r.isEmpty()) new AlertDialog.Builder(this).setTitle(title).setMessage(r).setPositiveButton("OK", null).show();
                if (after != null) after.run();
            });
        });
    }

    private void runSend(List<Uri> uris, String dir, boolean convert, Map<Uri, String> romDirs) {
        if (needConnection()) return;
        runWithProgress("Отправка на приставку",
                ui -> Jobs.sendToConsole(this, console, uris, dir, convert, romDirs, ui),
                () -> loadDir(curDir));
    }

    private void sendSmart(List<Uri> uris) { runSend(uris, null, true, null); }

    private void saveToPhone(List<Uri> uris) {
        runWithProgress("Конвертация", ui -> Jobs.convertToPhone(this, uris, ui), null);
    }

    // ------------------------------------------------------------------ страница «Файлы»

    private View buildFilesPage() {
        LinearLayout p = vbox();
        p.setPadding(dp(10), dp(4), dp(10), dp(4));
        pathText = text("/", 16);
        pathText.setTypeface(null, Typeface.BOLD);
        pathText.setTextColor(C_ACCENT);
        p.addView(pathText);
        freeText = text("", 12);
        freeText.setTextColor(C_DIM);
        p.addView(freeText);

        LinearLayout row = hbox();
        row.addView(button("↑ Назад", v -> goUp()), weight());
        row.addView(button("Обновить", v -> loadDir(curDir)), weight());
        p.addView(row);
        LinearLayout row2 = hbox();
        row2.addView(button("Загрузить сюда", v -> { if (!needConnection()) pick(REQ_UPLOAD_HERE); }), weight());
        row2.addView(button("Новая папка", v -> newFolder()), weight());
        p.addView(row2);
        p.addView(button("Скачать всю эту папку на телефон", v -> downloadHere()));
        convertSwitch = new Switch(this);
        convertSwitch.setText("Конвертировать видео, фото и музыку для приставки");
        convertSwitch.setChecked(sp.getBoolean("convertHere", true));
        convertSwitch.setOnCheckedChangeListener((b, c) -> sp.edit().putBoolean("convertHere", c).apply());
        p.addView(convertSwitch);

        ListView lv = new ListView(this);
        lv.setDivider(new android.graphics.drawable.ColorDrawable(C_LINE));
        lv.setDividerHeight(Math.max(1, dp(1)));
        entryAdapter = new BaseAdapter() {
            @Override public int getCount() { return entries.size(); }
            @Override public Object getItem(int i) { return entries.get(i); }
            @Override public long getItemId(int i) { return i; }
            @Override public View getView(int i, View cv, ViewGroup parent) {
                LinearLayout l = cv instanceof LinearLayout ? (LinearLayout) cv : null;
                if (l == null) {
                    l = vbox();
                    l.setPadding(dp(16), dp(12), dp(16), dp(12));
                    l.addView(text("", 16));
                    TextView sub = text("", 12);
                    sub.setTextColor(C_DIM);
                    l.addView(sub);
                    
                    TypedValue outValue = new TypedValue();
                    getTheme().resolveAttribute(android.R.attr.selectableItemBackground, outValue, true);
                    l.setBackgroundResource(outValue.resourceId);
                }
                Console.Entry e = entries.get(i);
                TextView nm = (TextView) l.getChildAt(0);
                nm.setText((e.dir ? "📁  " : iconFor(e.name) + "  ") + e.name);
                nm.setTextColor(e.dir ? C_DIR : C_TEXT);
                ((TextView) l.getChildAt(1)).setText(e.dir ? "папка" : Kinds.human(e.size));
                return l;
            }
        };
        lv.setAdapter(entryAdapter);
        lv.setOnItemClickListener((a, v, i, id) -> {
            Console.Entry e = entries.get(i);
            if (e.dir) loadDir(join(curDir, e.name)); else fileMenu(e);
        });
        lv.setOnItemLongClickListener((a, v, i, id) -> { fileMenu(entries.get(i)); return true; });
        p.addView(lv, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        return p;
    }

    private static String iconFor(String name) {
        String e = Kinds.ext(name);
        if (e.matches("avi|mjpeg|mjpg")) return "🎬";
        if (e.matches("jpg|jpeg|png|bmp|webp")) return "🖼";
        if (e.matches("mp3|wav")) return "🎵";
        if (Kinds.ROM_DIRS.containsKey(e) || e.equals("zip")) return "🎮";
        if (e.matches("txt|ini|md|csv")) return "📝";
        return "📄";
    }

    private static String join(String dir, String name) { return (dir.endsWith("/") ? dir : dir + "/") + name; }

    private void goUp() {
        if (curDir.equals("/")) return;
        int s = curDir.lastIndexOf('/');
        loadDir(s <= 0 ? "/" : curDir.substring(0, s));
    }

    private void loadDir(String dir) {
        if (!console.isConnected()) return;
        bg.execute(() -> {
            try {
                List<Console.Entry> l = console.list(dir);
                Console.Info info = console.info();
                main.post(() -> {
                    curDir = dir;
                    pathText.setText(dir);
                    entries.clear();
                    entries.addAll(l);
                    entryAdapter.notifyDataSetChanged();
                    freeText.setText(info == null ? "Файлов: " + l.size()
                            : "Свободно " + Kinds.human(info.free) + " из " + Kinds.human(info.total) + " · CrokOS " + info.ver);
                });
            } catch (Exception e) {
                main.post(() -> toast("Не удалось открыть " + dir + ": " + e.getMessage()));
            }
        });
    }

    private void newFolder() {
        if (needConnection()) return;
        EditText ed = new EditText(this);
        ed.setSingleLine();
        new AlertDialog.Builder(this).setTitle("Новая папка").setView(padded(ed))
                .setPositiveButton("Создать", (d, w) -> {
                    String n = Kinds.safeName(ed.getText().toString());
                    bg.execute(() -> {
                        try { console.mkdir(join(curDir, n)); main.post(() -> loadDir(curDir)); }
                        catch (Exception e) { main.post(() -> toast(e.getMessage())); }
                    });
                })
                .setNegativeButton("Отмена", null).show();
    }

    private void fileMenu(Console.Entry e) {
        String path = join(curDir, e.name);
        List<String> items = new ArrayList<>();
        items.add(e.dir ? "Скачать папку на телефон" : "Скачать на телефон");
        items.add("Переименовать");
        items.add("Удалить");
        new AlertDialog.Builder(this).setTitle(e.name)
                .setItems(items.toArray(new String[0]), (d, w) -> {
                    String it = items.get(w);
                    if (it.startsWith("Скачать")) download(e, path);
                    else if (it.startsWith("Переименовать")) renameDialog(e, path);
                    else confirmDelete(e, path);
                }).show();
    }

    private void download(Console.Entry e, String path) {
        runWithProgress("Скачивание", ui -> Jobs.downloadTree(this, console, path, e.dir, e.size, ui), null);
    }

    private void downloadHere() {
        if (needConnection()) return;
        new AlertDialog.Builder(this)
                .setTitle("Скачать всё из " + curDir + "?")
                .setMessage("Все файлы и вложенные папки скопируются в «Загрузки/CrokPocket» на телефоне. "
                        + "Скорость около 0,3-1 МБ/с, большие папки (игры, видео) займут время.")
                .setPositiveButton("Скачать", (d, w) -> runWithProgress("Скачивание",
                        ui -> Jobs.downloadTree(this, console, curDir, true, 0, ui), null))
                .setNegativeButton("Отмена", null).show();
    }

    private void renameDialog(Console.Entry e, String path) {
        EditText ed = new EditText(this);
        ed.setSingleLine();
        ed.setText(e.name);
        new AlertDialog.Builder(this).setTitle("Переименовать").setView(padded(ed))
                .setPositiveButton("OK", (d, w) -> {
                    String n = Kinds.safeName(ed.getText().toString());
                    bg.execute(() -> {
                        try { console.rename(path, join(curDir, n)); main.post(() -> loadDir(curDir)); }
                        catch (Exception ex) {
                            main.post(() -> toast("Не удалось: " + ex.getMessage() + " (нужна прошивка CrokOS от 16.10.2026)"));
                        }
                    });
                })
                .setNegativeButton("Отмена", null).show();
    }

    private void confirmDelete(Console.Entry e, String path) {
        new AlertDialog.Builder(this).setTitle("Удалить?")
                .setMessage(e.dir ? "Папка «" + e.name + "» будет удалена вместе со всем содержимым." : "Файл «" + e.name + "» будет удалён с карты.")
                .setPositiveButton("Удалить", (d, w) -> bg.execute(() -> {
                    try { console.delete(path); main.post(() -> loadDir(curDir)); }
                    catch (Exception ex) { main.post(() -> toast(ex.getMessage())); }
                }))
                .setNegativeButton("Отмена", null).show();
    }

    // ------------------------------------------------------------------ страница «Конвертер»

    private View buildConvertPage() {
        LinearLayout p = vbox();
        p.setPadding(dp(14), dp(8), dp(14), dp(14));
        p.addView(text("Телефон сам переводит файлы в форматы, которые понимает приставка, и раскладывает их по папкам:", 14));
        p.addView(text("• видео (MP4 и любое другое) → AVI 320×240, MJPEG + звук → /Video\n"
                + "• фото (JPG, PNG, HEIC, WebP) → JPEG до 640×480 → /Pictures\n"
                + "• музыка: MP3 как есть, остальное → WAV → /Music\n"
                + "• игры → /roms/<система>, остальное → /Files", 13));
        p.addView(gap());

        p.addView(label("Кадров в секунду"));
        Integer[] fpsOpts = {10, 12, 15, 20, 24};
        Spinner fps = new Spinner(this);
        fps.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, fpsOpts));
        int curFps = sp.getInt("fps", 15);
        for (int i = 0; i < fpsOpts.length; i++) if (fpsOpts[i] == curFps) fps.setSelection(i);
        fps.setOnItemSelectedListener(new SimpleSelect(i -> sp.edit().putInt("fps", fpsOpts[i]).apply()));
        p.addView(fps);

        p.addView(label("Качество картинки"));
        String[] qNames = {"Низкое (меньше файл)", "Среднее", "Высокое"};
        int[] qVals = {50, 70, 85};
        Spinner q = new Spinner(this);
        q.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, qNames));
        int curQ = sp.getInt("quality", 70);
        q.setSelection(curQ <= 55 ? 0 : curQ <= 75 ? 1 : 2);
        q.setOnItemSelectedListener(new SimpleSelect(i -> sp.edit().putInt("quality", qVals[i]).apply()));
        p.addView(q);

        Switch fill = new Switch(this);
        fill.setText("Заполнять весь экран (края обрезаются)");
        fill.setChecked(sp.getBoolean("fill", false));
        fill.setOnCheckedChangeListener((b, c) -> sp.edit().putBoolean("fill", c).apply());
        p.addView(fill);
        p.addView(gap());

        p.addView(button("Выбрать файлы и отправить на приставку", v -> { if (!needConnection()) pick(REQ_SEND_SMART); }));
        p.addView(button("Выбрать файлы и сохранить на телефон", v -> pick(REQ_SAVE_PHONE)));
        p.addView(gap());
        p.addView(text("Конвертация видео идёт на самом телефоне: минута ролика - примерно 20-60 секунд. "
                + "Не сворачивайте приложение, пока идёт обработка. Из галереи или файлового менеджера можно сразу "
                + "нажать «Поделиться» → CrokPocket.", 12));
        return scroll(p);
    }

    // ------------------------------------------------------------------ страница «Игры»

    private View buildGamesPage() {
        LinearLayout p = vbox();
        p.setPadding(dp(14), dp(8), dp(14), dp(14));
        p.addView(text("Игры раскладываются по папкам Retro-Go по расширению файла. "
                + "ZIP с одной игрой тоже подходит: система определяется по файлу внутри.", 14));
        p.addView(gap());
        StringBuilder sb = new StringBuilder();
        for (Map.Entry<String, String> s : Kinds.SYSTEMS.entrySet()) {
            StringBuilder ex = new StringBuilder();
            for (Map.Entry<String, String> r : Kinds.ROM_DIRS.entrySet())
                if (r.getValue().equals(s.getKey())) ex.append(ex.length() == 0 ? "" : ", ").append('.').append(r.getKey());
            sb.append("• ").append(s.getValue()).append(": ").append(ex).append(" → /roms/").append(s.getKey()).append('\n');
        }
        p.addView(text(sb.toString().trim(), 13));
        p.addView(gap());
        p.addView(button("Выбрать игры и отправить", v -> { if (!needConnection()) pick(REQ_GAMES); }));
        p.addView(button("Открыть /roms на приставке", v -> {
            if (needConnection()) return;
            showPage(0);
            loadDir("/roms");
        }));
        return scroll(p);
    }

    private void sendGames(List<Uri> uris) {
        // определяем системы в фоне (ZIP нужно открыть), неизвестные - спрашиваем один раз
        bg.execute(() -> {
            Map<Uri, String> dirs = new HashMap<>();
            List<Uri> unknown = new ArrayList<>();
            for (Uri u : uris) {
                String name = Kinds.displayName(getContentResolver(), u);
                String d = Kinds.romDir(getContentResolver(), u, name);
                if (d != null) dirs.put(u, d); else unknown.add(u);
            }
            main.post(() -> {
                if (unknown.isEmpty()) { runSend(uris, null, false, dirs); return; }
                String[] keys = Kinds.SYSTEMS.keySet().toArray(new String[0]);
                String[] labels = Kinds.SYSTEMS.values().toArray(new String[0]);
                new AlertDialog.Builder(this)
                        .setTitle("Система для " + unknown.size() + " файл(ов) не определена")
                        .setItems(labels, (d, w) -> {
                            for (Uri u : unknown) dirs.put(u, keys[w]);
                            runSend(uris, null, false, dirs);
                        })
                        .setNegativeButton("Пропустить их", (d, w) -> {
                            List<Uri> known = new ArrayList<>(uris);
                            known.removeAll(unknown);
                            if (!known.isEmpty()) runSend(known, null, false, dirs);
                        })
                        .show();
            });
        });
    }

    // ------------------------------------------------------------------ страница «Настройки»

    private static final String[][] INI_FIELDS = {
            {"wifi_ssid", "Wi-Fi: имя сети"},
            {"wifi_password", "Wi-Fi: пароль"},
            {"api_url", "Свой сервис: адрес API"},
            {"api_key", "Свой сервис: ключ"},
            {"api_model", "Свой сервис: модель"},
            {"openrouter_key", "OpenRouter: ключ"},
            {"openrouter_model", "OpenRouter: модель"},
            {"gemini_key", "Gemini: ключ"},
            {"model", "Gemini: модель"},
            {"relay", "Свой сервер: адрес:порт"},
            {"proxy", "Прокси (Cloudflare Workers)"},
    };

    private View buildSettingsPage() {
        LinearLayout p = vbox();
        p.setPadding(dp(14), dp(8), dp(14), dp(14));
        p.addView(text("Настройки нейросети и Wi-Fi приставки. Кнопка внизу создаёт файл crokos.ini и кладёт его в корень "
                + "SD-карты: приставка прочитает его при следующем открытии «Нейросети». Пустые поля не меняют то, "
                + "что уже настроено на приставке.", 13));
        p.addView(gap());

        String[] provVals = {"", "gemini", "openrouter", "api"};
        String[] provNames = {"Не менять", "Gemini (Google)", "OpenRouter", "Свой сервис (UnoRouter и др.)"};
        p.addView(label("Нейросеть"));
        Spinner prov = new Spinner(this);
        prov.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, provNames));
        prov.setSelection(indexOf(provVals, sp.getString("ini_provider", "")));
        prov.setOnItemSelectedListener(new SimpleSelect(i -> sp.edit().putString("ini_provider", provVals[i]).apply()));
        p.addView(prov);

        String[] modeVals = {"", "direct", "relay", "proxy"};
        String[] modeNames = {"Не менять", "Напрямую", "Через свой сервер (VPN)", "Через прокси (только Gemini)"};
        p.addView(label("Подключение"));
        Spinner mode = new Spinner(this);
        mode.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, modeNames));
        mode.setSelection(indexOf(modeVals, sp.getString("ini_mode", "")));
        mode.setOnItemSelectedListener(new SimpleSelect(i -> sp.edit().putString("ini_mode", modeVals[i]).apply()));
        p.addView(mode);

        Map<String, EditText> eds = new HashMap<>();
        for (String[] f : INI_FIELDS) {
            p.addView(text(f[1], 13));
            EditText ed = new EditText(this);
            ed.setSingleLine();
            boolean secret = f[0].endsWith("key") || f[0].endsWith("password");
            ed.setInputType(InputType.TYPE_CLASS_TEXT | (secret ? InputType.TYPE_TEXT_VARIATION_PASSWORD : InputType.TYPE_TEXT_VARIATION_URI));
            ed.setText(sp.getString("ini_" + f[0], ""));
            eds.put(f[0], ed);
            p.addView(ed);
        }
        p.addView(gap());
        p.addView(button("Отправить настройки на приставку", v -> {
            SharedPreferences.Editor e = sp.edit();
            for (String[] f : INI_FIELDS) e.putString("ini_" + f[0], eds.get(f[0]).getText().toString().trim());
            e.apply();
            sendIni();
        }));
        p.addView(button("Стереть сохранённые на телефоне ключи и пароли", v -> {
            SharedPreferences.Editor e = sp.edit();
            for (String[] f : INI_FIELDS) {
                if (f[0].endsWith("key") || f[0].endsWith("password")) { e.remove("ini_" + f[0]); eds.get(f[0]).setText(""); }
            }
            e.apply();
            toast("Ключи и пароли удалены с телефона");
        }));
        p.addView(gap());
        p.addView(text("Ключи хранятся только в этом приложении. С приставки файл crokos.ini скачать нельзя: "
                + "сеть CrokPocket открытая, поэтому приставка его не отдаёт.", 12));
        return scroll(p);
    }

    private static int indexOf(String[] a, String v) {
        for (int i = 0; i < a.length; i++) if (a[i].equals(v)) return i;
        return 0;
    }

    private void sendIni() {
        if (needConnection()) return;
        StringBuilder ini = new StringBuilder("# Создано приложением CrokPocket\n");
        String prov = sp.getString("ini_provider", "");
        if (!prov.isEmpty()) ini.append("provider=").append(prov).append('\n');
        String mode = sp.getString("ini_mode", "");
        if (!mode.isEmpty()) ini.append("mode=").append(mode).append('\n');
        int n = 0;
        for (String[] f : INI_FIELDS) {
            String v = sp.getString("ini_" + f[0], "");
            if (!v.isEmpty()) { ini.append(f[0]).append('=').append(v).append('\n'); n++; }
        }
        if (prov.isEmpty() && mode.isEmpty() && n == 0) { toast("Заполните хотя бы одно поле"); return; }
        byte[] data = ini.toString().getBytes(StandardCharsets.UTF_8);
        runWithProgress("Настройки", ui -> {
            console.upload("/", "crokos.ini", new ByteArrayInputStream(data), data.length, null);
            return "Файл crokos.ini отправлен. Откройте на приставке «Нейросеть» - настройки применятся.";
        }, null);
    }

    // ------------------------------------------------------------------ мелочи интерфейса

    private interface IntCb { void on(int i); }

    private static final class SimpleSelect implements android.widget.AdapterView.OnItemSelectedListener {
        private final IntCb cb;
        SimpleSelect(IntCb cb) { this.cb = cb; }
        @Override public void onItemSelected(android.widget.AdapterView<?> a, View v, int i, long id) { cb.on(i); }
        @Override public void onNothingSelected(android.widget.AdapterView<?> a) { }
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private LinearLayout vbox() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        return l;
    }

    private LinearLayout hbox() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.HORIZONTAL);
        return l;
    }

    private LinearLayout.LayoutParams weight() {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1);
        p.setMargins(dp(4), dp(4), dp(4), dp(4));
        return p;
    }

    private TextView text(String s, int sp) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setPadding(0, dp(2), 0, dp(2));
        return t;
    }

    /** Подпись раздела - оранжевая, как путь на сайте приставки */
    private TextView label(String s) {
        TextView t = text(s, 13);
        t.setTextColor(C_ACCENT);
        t.setPadding(0, dp(8), 0, dp(2));
        return t;
    }

    private int getThemeColor(int attr) {
        TypedValue tv = new TypedValue();
        getTheme().resolveAttribute(attr, tv, true);
        return tv.data;
    }

    private TextView tabView(String s, View.OnClickListener l) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setGravity(Gravity.CENTER);
        t.setPadding(0, dp(12), 0, dp(12));
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        t.setOnClickListener(l);
        return t;
    }

    private Button button(String s, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setOnClickListener(l);
        b.setTextColor(0xFFEEEEEE);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        b.setStateListAnimator(null);   // без тени Material - плоские кнопки как на сайте

        GradientDrawable gd = new GradientDrawable();
        gd.setColor(C_BTN);
        gd.setStroke(Math.max(1, dp(1)), C_BORDER);
        gd.setCornerRadius(dp(6));

        GradientDrawable gdPressed = new GradientDrawable();
        gdPressed.setColor(C_BTN_DOWN);
        gdPressed.setStroke(Math.max(1, dp(1)), C_ACCENT);
        gdPressed.setCornerRadius(dp(6));
        
        StateListDrawable states = new StateListDrawable();
        states.addState(new int[]{android.R.attr.state_pressed}, gdPressed);
        states.addState(new int[]{}, gd);
        
        b.setBackground(states);
        b.setPadding(dp(14), dp(10), dp(14), dp(10));
        b.setMinHeight(0);
        b.setMinimumHeight(0);
        
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        params.setMargins(dp(4), dp(4), dp(4), dp(4));
        b.setLayoutParams(params);
        
        return b;
    }

    private View gap() {
        View v = new View(this);
        v.setLayoutParams(new LinearLayout.LayoutParams(1, dp(10)));
        return v;
    }

    private View scroll(View inner) {
        ScrollView s = new ScrollView(this);
        s.addView(inner);
        return s;
    }

    private View padded(View v) {
        FrameLayout f = new FrameLayout(this);
        f.setPadding(dp(20), dp(8), dp(20), 0);
        f.addView(v);
        return f;
    }

    private void toast(String s) { Toast.makeText(this, s, Toast.LENGTH_LONG).show(); }
}

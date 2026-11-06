package ru.crokpocket.app;

import android.content.ContentResolver;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ImageDecoder;
import android.graphics.Rect;
import android.media.AudioFormat;
import android.media.Image;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.net.Uri;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.nio.ShortBuffer;

/**
 * Конвертация под приставку средствами самого Android (MediaCodec), без ffmpeg:
 *  видео  -> AVI: MJPEG (JPEG-кадры) до 320x240 + звук PCM 16 бит моно 22050 Гц
 *  фото   -> JPEG (baseline) до 640x480, с учётом поворота из EXIF
 *  музыка -> MP3 остаётся как есть, остальное -> WAV 16 бит моно 32000 Гц
 */
public final class Media {
    private Media() { }

    public interface Prog {
        /** @return false - отменить */
        boolean on(int percent, String stage);
    }

    public static class CancelledException extends IOException {
        public CancelledException() { super("Отменено"); }
    }

    public static class VideoOpts {
        public int fps = 15;
        public int quality = 70;       // качество JPEG 1-100
        public boolean fill = false;   // true - заполнить экран (обрезать края), false - вписать целиком
        public int maxW = 320, maxH = 240;
        public int audioRate = 22050;
    }

    public static final int IMAGE_MAX_W = 640, IMAGE_MAX_H = 480;
    public static final int MUSIC_RATE = 32000;

    private static void check(Prog p, int pct, String stage) throws CancelledException {
        if (p != null && !p.on(Math.max(0, Math.min(100, pct)), stage)) throw new CancelledException();
    }

    private static int findTrack(MediaExtractor ex, String prefix) {
        for (int i = 0; i < ex.getTrackCount(); i++) {
            String m = ex.getTrackFormat(i).getString(MediaFormat.KEY_MIME);
            if (m != null && m.startsWith(prefix)) return i;
        }
        return -1;
    }

    // ------------------------------------------------------------------ звук

    /** Линейный ресемплер в моно 16 бит, пишет little-endian в поток */
    private static final class Resampler {
        private final OutputStream out;
        private final int dstRate;
        private double step, t = 0;
        private long idx = -1;
        private float prev = 0, lp = 0;
        private boolean lowpass;
        private final byte[] buf = new byte[8192];
        private int bl = 0;
        long count = 0;

        Resampler(int srcRate, int dstRate, OutputStream out) {
            this.out = out;
            this.dstRate = dstRate;
            setSrcRate(srcRate);
        }

        void setSrcRate(int srcRate) {
            step = (double) srcRate / dstRate;
            lowpass = step > 1.3;
        }

        void push(float s) throws IOException {
            if (lowpass) { lp = lp * 0.45f + s * 0.55f; s = lp; }   // простое сглаживание против «звона» при понижении частоты
            if (idx < 0) {
                idx = 0;
                prev = s;
                while (t <= 0) { emit(s); t += step; }
                return;
            }
            idx++;
            while (t <= idx) {
                double frac = t - (idx - 1);
                emit((float) (prev + (s - prev) * frac));
                t += step;
            }
            prev = s;
        }

        private void emit(float v) throws IOException {
            int x = Math.round(v);
            if (x > 32767) x = 32767; else if (x < -32768) x = -32768;
            buf[bl++] = (byte) x;
            buf[bl++] = (byte) (x >> 8);
            count++;
            if (bl == buf.length) { out.write(buf, 0, bl); bl = 0; }
        }

        void flush() throws IOException { if (bl > 0) out.write(buf, 0, bl); bl = 0; out.flush(); }
    }

    /**
     * Декодирует звуковую дорожку в моно PCM 16 бит с частотой dstRate.
     * @return число отсчётов или -1, если звука нет
     */
    public static long decodeAudio(Context ctx, Uri uri, int dstRate, OutputStream out,
                                   Prog p, int pFrom, int pTo, String stage) throws Exception {
        MediaExtractor ex = new MediaExtractor();
        MediaCodec dec = null;
        try {
            ex.setDataSource(ctx, uri, null);
            int t = findTrack(ex, "audio/");
            if (t < 0) return -1;
            MediaFormat f = ex.getTrackFormat(t);
            long dur = f.containsKey(MediaFormat.KEY_DURATION) ? f.getLong(MediaFormat.KEY_DURATION) : 0;
            ex.selectTrack(t);
            dec = MediaCodec.createDecoderByType(f.getString(MediaFormat.KEY_MIME));
            dec.configure(f, null, null, 0);
            dec.start();
            int ch = f.containsKey(MediaFormat.KEY_CHANNEL_COUNT) ? f.getInteger(MediaFormat.KEY_CHANNEL_COUNT) : 2;
            int rate = f.containsKey(MediaFormat.KEY_SAMPLE_RATE) ? f.getInteger(MediaFormat.KEY_SAMPLE_RATE) : 44100;
            int enc = AudioFormat.ENCODING_PCM_16BIT;
            Resampler rs = new Resampler(rate, dstRate, out);
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            boolean inEos = false, outEos = false;
            int lastPct = -1;
            while (!outEos) {
                if (!inEos) {
                    int ii = dec.dequeueInputBuffer(10000);
                    if (ii >= 0) {
                        ByteBuffer b = dec.getInputBuffer(ii);
                        int n = b == null ? -1 : ex.readSampleData(b, 0);
                        if (n < 0) {
                            dec.queueInputBuffer(ii, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            inEos = true;
                        } else {
                            dec.queueInputBuffer(ii, 0, n, ex.getSampleTime(), 0);
                            ex.advance();
                        }
                    }
                }
                int oi = dec.dequeueOutputBuffer(info, 10000);
                if (oi == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    MediaFormat of = dec.getOutputFormat();
                    if (of.containsKey(MediaFormat.KEY_CHANNEL_COUNT)) ch = of.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
                    if (of.containsKey(MediaFormat.KEY_SAMPLE_RATE)) rate = of.getInteger(MediaFormat.KEY_SAMPLE_RATE);
                    if (of.containsKey(MediaFormat.KEY_PCM_ENCODING)) enc = of.getInteger(MediaFormat.KEY_PCM_ENCODING);
                    rs.setSrcRate(rate);
                } else if (oi >= 0) {
                    ByteBuffer ob = dec.getOutputBuffer(oi);
                    if (ob != null && info.size > 0) {
                        ob.position(info.offset);
                        ob.limit(info.offset + info.size);
                        ByteBuffer le = ob.slice().order(ByteOrder.LITTLE_ENDIAN);
                        int c = Math.max(1, ch);
                        if (enc == AudioFormat.ENCODING_PCM_FLOAT) {
                            FloatBuffer fb = le.asFloatBuffer();
                            int frames = fb.remaining() / c;
                            for (int i = 0; i < frames; i++) {
                                float s = 0;
                                for (int k = 0; k < c; k++) s += fb.get();
                                rs.push(s / c * 32767f);
                            }
                        } else {
                            ShortBuffer sb = le.asShortBuffer();
                            int frames = sb.remaining() / c;
                            for (int i = 0; i < frames; i++) {
                                int s = 0;
                                for (int k = 0; k < c; k++) s += sb.get();
                                rs.push((float) s / c);
                            }
                        }
                    }
                    dec.releaseOutputBuffer(oi, false);
                    if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) outEos = true;
                    if (dur > 0) {
                        int pct = pFrom + (int) ((pTo - pFrom) * Math.min(1.0, (double) info.presentationTimeUs / dur));
                        if (pct != lastPct) { check(p, pct, stage); lastPct = pct; }
                    }
                }
            }
            rs.flush();
            return rs.count;
        } finally {
            if (dec != null) { try { dec.stop(); } catch (Exception ignored) { } dec.release(); }
            ex.release();
        }
    }

    // ------------------------------------------------------------------ музыка

    public static boolean isMp3(ContentResolver cr, Uri uri, String name) {
        String m = null;
        try { m = cr.getType(uri); } catch (Exception ignored) { }
        return "audio/mpeg".equals(m) || Kinds.ext(name).equals("mp3");
    }

    /** Любой звук -> WAV моно 16 бит 32 кГц */
    public static void convertAudio(Context ctx, Uri uri, File out, Prog p) throws Exception {
        try (RandomAccessFile raf = new RandomAccessFile(out, "rw")) {
            raf.setLength(0);
            raf.write(new byte[44]);
            long n;
            try (OutputStream os = new BufferedOutputStream(new FileOutputStream(raf.getFD()), 65536)) {
                n = decodeAudio(ctx, uri, MUSIC_RATE, os, p, 0, 100, "Музыка");
            }
            if (n <= 0) throw new IOException("В файле нет звука");
            raf.seek(0);
            raf.write(wavHeader(MUSIC_RATE, n));
        }
    }

    private static byte[] wavHeader(int rate, long samples) {
        ByteBuffer b = ByteBuffer.allocate(44).order(ByteOrder.LITTLE_ENDIAN);
        long data = samples * 2;
        b.put("RIFF".getBytes()).putInt((int) (36 + data)).put("WAVE".getBytes());
        b.put("fmt ".getBytes()).putInt(16).putShort((short) 1).putShort((short) 1)
                .putInt(rate).putInt(rate * 2).putShort((short) 2).putShort((short) 16);
        b.put("data".getBytes()).putInt((int) data);
        return b.array();
    }

    // ------------------------------------------------------------------ фото

    public static void convertImage(Context ctx, Uri uri, File out) throws Exception {
        ImageDecoder.Source src = ImageDecoder.createSource(ctx.getContentResolver(), uri);
        Bitmap bmp = ImageDecoder.decodeBitmap(src, (dec, info, s) -> {
            int w = info.getSize().getWidth(), h = info.getSize().getHeight();
            double sc = Math.min(1.0, Math.min((double) IMAGE_MAX_W / w, (double) IMAGE_MAX_H / h));
            dec.setTargetSize(Math.max(1, (int) Math.round(w * sc)), Math.max(1, (int) Math.round(h * sc)));
            dec.setAllocator(ImageDecoder.ALLOCATOR_SOFTWARE);
        });
        // прозрачность (PNG, стикеры) -> на чёрном фоне, JPEG прозрачность не хранит
        Bitmap rgb = Bitmap.createBitmap(bmp.getWidth(), bmp.getHeight(), Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(rgb);
        c.drawColor(Color.BLACK);
        c.drawBitmap(bmp, 0, 0, null);
        bmp.recycle();
        try (OutputStream os = new BufferedOutputStream(new FileOutputStream(out))) {
            rgb.compress(Bitmap.CompressFormat.JPEG, 88, os);
        }
        rgb.recycle();
    }

    // ------------------------------------------------------------------ видео

    /** Запись AVI (RIFF): поток 00dc - кадры JPEG, 01wb - PCM; в конце индекс idx1 для плееров на ПК */
    static final class AviWriter {
        private final File file;
        private final OutputStream os;
        private final int w, h, fps, rate;
        private final boolean audio;
        private final int hdrlSize, moviListPos;
        private long pos;
        private final ByteArrayOutputStream idx = new ByteArrayOutputStream();
        private int frames = 0, maxChunk = 0;
        private long audioBytes = 0;

        AviWriter(File file, int w, int h, int fps, int audioRate) throws IOException {
            this.file = file;
            this.w = w; this.h = h; this.fps = fps;
            this.rate = audioRate;
            this.audio = audioRate > 0;
            hdrlSize = 4 + (8 + 56) + (8 + 116) + (audio ? 8 + 94 : 0);
            moviListPos = 12 + 8 + hdrlSize;
            os = new BufferedOutputStream(new FileOutputStream(file), 1 << 16);
            byte[] hdr = header(0, 0, 0, 0, 0);
            os.write(hdr);
            pos = hdr.length;
        }

        private static void fcc(ByteBuffer b, String s) { b.put(s.getBytes()); }

        private byte[] header(int totalFrames, int moviSize, long riffSize, long audioLen, int maxBps) {
            ByteBuffer b = ByteBuffer.allocate(moviListPos + 12).order(ByteOrder.LITTLE_ENDIAN);
            fcc(b, "RIFF"); b.putInt((int) riffSize); fcc(b, "AVI ");
            fcc(b, "LIST"); b.putInt(hdrlSize); fcc(b, "hdrl");
            fcc(b, "avih"); b.putInt(56);
            b.putInt(1000000 / fps);          // мкс на кадр
            b.putInt(maxBps);                 // байт/с
            b.putInt(0);                      // гранулярность
            b.putInt(0x10 | 0x100);           // AVIF_HASINDEX | AVIF_ISINTERLEAVED
            b.putInt(totalFrames);
            b.putInt(0);
            b.putInt(audio ? 2 : 1);
            b.putInt(maxChunk + 8);
            b.putInt(w); b.putInt(h);
            b.putInt(0); b.putInt(0); b.putInt(0); b.putInt(0);
            // поток видео
            fcc(b, "LIST"); b.putInt(116); fcc(b, "strl");
            fcc(b, "strh"); b.putInt(56);
            fcc(b, "vids"); fcc(b, "MJPG");
            b.putInt(0); b.putShort((short) 0); b.putShort((short) 0); b.putInt(0);
            b.putInt(1); b.putInt(fps); b.putInt(0); b.putInt(totalFrames);
            b.putInt(maxChunk + 8); b.putInt(-1); b.putInt(0);
            b.putShort((short) 0); b.putShort((short) 0); b.putShort((short) w); b.putShort((short) h);
            fcc(b, "strf"); b.putInt(40);
            b.putInt(40); b.putInt(w); b.putInt(h); b.putShort((short) 1); b.putShort((short) 24);
            fcc(b, "MJPG"); b.putInt(w * h * 3); b.putInt(0); b.putInt(0); b.putInt(0); b.putInt(0);
            if (audio) {
                fcc(b, "LIST"); b.putInt(94); fcc(b, "strl");
                fcc(b, "strh"); b.putInt(56);
                fcc(b, "auds"); b.putInt(0);
                b.putInt(0); b.putShort((short) 0); b.putShort((short) 0); b.putInt(0);
                b.putInt(2); b.putInt(rate * 2); b.putInt(0); b.putInt((int) audioLen);
                b.putInt(rate * 2 / fps * 2); b.putInt(-1); b.putInt(2);
                b.putShort((short) 0); b.putShort((short) 0); b.putShort((short) 0); b.putShort((short) 0);
                fcc(b, "strf"); b.putInt(18);
                b.putShort((short) 1); b.putShort((short) 1); b.putInt(rate); b.putInt(rate * 2);
                b.putShort((short) 2); b.putShort((short) 16); b.putShort((short) 0);
            }
            fcc(b, "LIST"); b.putInt(moviSize); fcc(b, "movi");
            return b.array();
        }

        private void chunk(String id, byte[] data, int len, boolean key) throws IOException {
            ByteBuffer e = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN);
            fcc(e, id);
            e.putInt(key ? 0x10 : 0);
            e.putInt((int) (pos - (moviListPos + 8)));
            e.putInt(len);
            idx.write(e.array());
            ByteBuffer hd = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN);
            fcc(hd, id);
            hd.putInt(len);
            os.write(hd.array());
            os.write(data, 0, len);
            pos += 8 + len;
            if ((len & 1) != 0) { os.write(0); pos++; }
            if (len > maxChunk) maxChunk = len;
        }

        void frame(byte[] jpeg, int len) throws IOException { chunk("00dc", jpeg, len, true); frames++; }

        void audio(byte[] pcm, int len) throws IOException { chunk("01wb", pcm, len, true); audioBytes += len; }

        void finish() throws IOException {
            ByteBuffer hd = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN);
            fcc(hd, "idx1");
            hd.putInt(idx.size());
            long idxPos = pos;
            os.write(hd.array());
            idx.writeTo(os);
            pos += 8 + idx.size();
            os.close();
            int moviSize = (int) (idxPos - (moviListPos + 8));
            int maxBps = (int) Math.min(Integer.MAX_VALUE, (long) (maxChunk + 8) * fps + (audio ? rate * 2L : 0));
            byte[] hdr = header(frames, moviSize, pos - 8, audioBytes / 2, maxBps);
            try (RandomAccessFile raf = new RandomAccessFile(file, "rw")) {
                raf.seek(0);
                raf.write(hdr);
            }
        }
    }

    /** Кадр YUV -> RGB нужного размера с поворотом, масштабированием и усреднением яркости */
    private static final class Scaler {
        int outW, outH, rotation;
        boolean fill;
        int srcW = -1, srcH = -1;
        int[] mapX, mapY, argb;
        int box = 1;
        byte[] yA, uA, vA;

        Scaler(int outW, int outH, int rotation, boolean fill) {
            this.outW = outW; this.outH = outH; this.rotation = rotation; this.fill = fill;
            argb = new int[outW * outH];
        }

        private void prepare(int sw, int sh) {
            srcW = sw; srcH = sh;
            boolean swap = rotation == 90 || rotation == 270;
            int dw = swap ? sh : sw, dh = swap ? sw : sh;
            // область кадра (в координатах «как показывать»), которая попадёт на экран
            double sc = fill ? Math.max((double) outW / dw, (double) outH / dh) : Math.min((double) outW / dw, (double) outH / dh);
            double rw = outW / sc, rh = outH / sc;
            double x0 = (dw - rw) / 2, y0 = (dh - rh) / 2;
            box = Math.max(1, Math.min(4, (int) (1.0 / sc)));
            mapX = new int[outW * outH];
            mapY = new int[outW * outH];
            for (int y = 0; y < outH; y++) {
                for (int x = 0; x < outW; x++) {
                    int dx = (int) (x0 + (x + 0.5) / sc);
                    int dy = (int) (y0 + (y + 0.5) / sc);
                    dx = Math.max(0, Math.min(dw - 1, dx));
                    dy = Math.max(0, Math.min(dh - 1, dy));
                    int sx, sy;
                    switch (rotation) {
                        case 90: sx = dy; sy = sh - 1 - dx; break;
                        case 180: sx = sw - 1 - dx; sy = sh - 1 - dy; break;
                        case 270: sx = sw - 1 - dy; sy = dx; break;
                        default: sx = dx; sy = dy;
                    }
                    int i = y * outW + x;
                    mapX[i] = Math.max(0, Math.min(sw - box, sx));
                    mapY[i] = Math.max(0, Math.min(sh - box, sy));
                }
            }
        }

        private static byte[] copy(ByteBuffer b, byte[] arr) {
            b.rewind();
            int n = b.remaining();
            if (arr == null || arr.length < n) arr = new byte[n];
            b.get(arr, 0, n);
            return arr;
        }

        int[] convert(Image img) {
            Rect cr = img.getCropRect();
            int cx = cr.left, cy = cr.top, sw = cr.width(), sh = cr.height();
            if (sw != srcW || sh != srcH) prepare(sw, sh);
            Image.Plane[] pl = img.getPlanes();
            int hi = img.getFormat() == 0x36 ? 1 : 0;   // ImageFormat.YCBCR_P010 (10 бит): берём старший байт
            yA = copy(pl[0].getBuffer(), yA);
            uA = copy(pl[1].getBuffer(), uA);
            vA = copy(pl[2].getBuffer(), vA);
            int yRS = pl[0].getRowStride(), yPS = pl[0].getPixelStride();
            int uRS = pl[1].getRowStride(), uPS = pl[1].getPixelStride();
            int vRS = pl[2].getRowStride(), vPS = pl[2].getPixelStride();
            int n = outW * outH, bb = box * box;
            for (int i = 0; i < n; i++) {
                int sx = mapX[i] + cx, sy = mapY[i] + cy;
                int ys = 0;
                for (int ky = 0; ky < box; ky++) {
                    int row = (sy + ky) * yRS + hi;
                    for (int kx = 0; kx < box; kx++) ys += yA[row + (sx + kx) * yPS] & 0xFF;
                }
                int Y = ys / bb - 16;
                int ux = sx >> 1, uy = sy >> 1;
                int U = (uA[uy * uRS + ux * uPS + hi] & 0xFF) - 128;
                int V = (vA[uy * vRS + ux * vPS + hi] & 0xFF) - 128;
                int c = 298 * (Y < 0 ? 0 : Y);
                int r = (c + 409 * V + 128) >> 8;
                int g = (c - 100 * U - 208 * V + 128) >> 8;
                int b = (c + 516 * U + 128) >> 8;
                r = r < 0 ? 0 : (r > 255 ? 255 : r);
                g = g < 0 ? 0 : (g > 255 ? 255 : g);
                b = b < 0 ? 0 : (b > 255 ? 255 : b);
                argb[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
            }
            return argb;
        }
    }

    public static void convertVideo(Context ctx, Uri uri, File out, VideoOpts o, Prog p) throws Exception {
        File pcm = File.createTempFile("audio", ".raw", ctx.getCacheDir());
        MediaExtractor ex = new MediaExtractor();
        MediaCodec dec = null;
        try {
            // 1. звук - во временный файл (моно 16 бит)
            long samples;
            try (OutputStream ao = new BufferedOutputStream(new FileOutputStream(pcm), 1 << 16)) {
                samples = decodeAudio(ctx, uri, o.audioRate, ao, p, 0, 15, "Звук");
            }
            boolean hasAudio = samples > 0;

            // 2. видео
            ex.setDataSource(ctx, uri, null);
            int t = findTrack(ex, "video/");
            if (t < 0) throw new IOException("В файле нет видеодорожки");
            MediaFormat f = ex.getTrackFormat(t);
            String mime = f.getString(MediaFormat.KEY_MIME);
            int sw = f.getInteger(MediaFormat.KEY_WIDTH), sh = f.getInteger(MediaFormat.KEY_HEIGHT);
            int rot = f.containsKey(MediaFormat.KEY_ROTATION) ? f.getInteger(MediaFormat.KEY_ROTATION) : 0;
            rot = ((rot % 360) + 360) % 360;
            long dur = f.containsKey(MediaFormat.KEY_DURATION) ? f.getLong(MediaFormat.KEY_DURATION) : 0;
            boolean swap = rot == 90 || rot == 270;
            int dw = swap ? sh : sw, dh = swap ? sw : sh;
            int W, H;
            if (o.fill) { W = o.maxW; H = o.maxH; }
            else {
                double sc = Math.min((double) o.maxW / dw, (double) o.maxH / dh);
                W = Math.max(2, ((int) Math.round(dw * sc)) & ~1);
                H = Math.max(2, ((int) Math.round(dh * sc)) & ~1);
            }
            Scaler scaler = new Scaler(W, H, rot, o.fill);
            Bitmap bmp = Bitmap.createBitmap(W, H, Bitmap.Config.ARGB_8888);
            ByteArrayOutputStream jpg = new ByteArrayOutputStream(64 * 1024);

            ex.selectTrack(t);
            f.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
            dec = MediaCodec.createDecoderByType(mime);
            dec.configure(f, null, null, 0);
            dec.start();

            AviWriter avi = new AviWriter(out, W, H, o.fps, hasAudio ? o.audioRate : 0);
            InputStream pin = hasAudio ? new BufferedInputStream(new FileInputStream(pcm), 1 << 16) : null;
            byte[] abuf = new byte[(o.audioRate / o.fps + 2) * 2];
            long audioDone = 0;
            final double frameUs = 1_000_000.0 / o.fps;
            long k = 0;           // номер следующего кадра AVI
            long firstPts = -1;
            byte[] lastJpeg = null;
            int lastLen = 0;
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            boolean inEos = false, outEos = false;
            int lastPct = -1;
            try {
                while (!outEos) {
                    if (!inEos) {
                        int ii = dec.dequeueInputBuffer(10000);
                        if (ii >= 0) {
                            ByteBuffer b = dec.getInputBuffer(ii);
                            int n = b == null ? -1 : ex.readSampleData(b, 0);
                            if (n < 0) {
                                dec.queueInputBuffer(ii, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                                inEos = true;
                            } else {
                                dec.queueInputBuffer(ii, 0, n, ex.getSampleTime(), 0);
                                ex.advance();
                            }
                        }
                    }
                    int oi = dec.dequeueOutputBuffer(info, 10000);
                    if (oi < 0) continue;
                    if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) outEos = true;
                    if (info.size <= 0) { dec.releaseOutputBuffer(oi, false); continue; }
                    if (firstPts < 0) firstPts = info.presentationTimeUs;
                    long pts = info.presentationTimeUs - firstPts;
                    if (k * frameUs <= pts) {
                        Image img = dec.getOutputImage(oi);
                        if (img != null) {
                            int[] px = scaler.convert(img);
                            img.close();
                            bmp.setPixels(px, 0, W, 0, 0, W, H);
                            jpg.reset();
                            bmp.compress(Bitmap.CompressFormat.JPEG, o.quality, jpg);
                            lastJpeg = jpg.toByteArray();
                            lastLen = lastJpeg.length;
                        }
                        // один кадр источника может закрыть несколько кадров AVI (низкий fps исходника)
                        while (lastJpeg != null && k * frameUs <= pts) {
                            avi.frame(lastJpeg, lastLen);
                            audioDone = writeAudio(avi, pin, abuf, k, o, audioDone);
                            k++;
                        }
                    }
                    dec.releaseOutputBuffer(oi, false);
                    if (dur > 0) {
                        int pct = 15 + (int) (85 * Math.min(1.0, (double) pts / dur));
                        if (pct != lastPct) { check(p, pct, "Видео"); lastPct = pct; }
                    }
                }
                // добиваем последним кадром до конца дорожки
                while (lastJpeg != null && dur > 0 && k * frameUs < dur) {
                    avi.frame(lastJpeg, lastLen);
                    audioDone = writeAudio(avi, pin, abuf, k, o, audioDone);
                    k++;
                }
                if (k == 0) throw new IOException("Не удалось декодировать ни одного кадра");
            } finally {
                if (pin != null) pin.close();
            }
            avi.finish();
            bmp.recycle();
            check(p, 100, "Готово");
        } catch (Exception e) {
            out.delete();
            throw e;
        } finally {
            if (dec != null) { try { dec.stop(); } catch (Exception ignored) { } dec.release(); }
            ex.release();
            pcm.delete();
        }
    }

    /** Звук к кадру k: столько отсчётов, чтобы к концу кадра набралось ровно (k+1)/fps секунды */
    private static long writeAudio(AviWriter avi, InputStream pin, byte[] buf, long k, VideoOpts o, long done) throws IOException {
        if (pin == null) return done;
        long need = (k + 1) * o.audioRate / o.fps - done;
        if (need <= 0) return done;
        int bytes = (int) Math.min(buf.length, need * 2);
        int got = 0;
        while (got < bytes) {
            int r = pin.read(buf, got, bytes - got);
            if (r <= 0) break;
            got += r;
        }
        for (int i = got; i < bytes; i++) buf[i] = 0;   // звук кончился раньше видео - тишина
        avi.audio(buf, bytes);
        return done + bytes / 2;
    }
}

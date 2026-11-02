package com.wtboard;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.app.AlertDialog;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanRecord;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.TextView;

import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.Map;
import java.util.UUID;

// Shifts a WHEELTOP EDS rear derailleur straight from the phone, the way the Wheeltop app does
// (protocol: section 19 of wheeltop_eds_protocol_spec.md), and configures the WT Board
// (esp32/wheeltop_pod): its mode and its pairing with the derailleur.
//
// The board advertises as "WT Board" with a settings service: the command characteristic takes
// "u", "d", "pair", "mode app", "mode pod"; the status characteristic reports key=value pairs
// separated by ';' (mode, paired, rd, ready, pairing, yield, gear, addr); the event characteristic
// notifies "u"/"d" when a board button is pressed.
//
// The derailleur takes only one app at a time. While this app is subscribed to the board's events, a board in
// phone mode disconnects from the derailleur (yield=1) and sends its button presses here, and the phone shifts.
@SuppressLint("MissingPermission") // permissions are requested in onCreate before any Bluetooth call
public class MainActivity extends Activity {
    static final UUID BOARD_SERVICE = UUID.fromString("b5f90001-1a2b-4c3d-8e9f-0a1b2c3d4e5f");
    static final UUID BOARD_COMMAND = UUID.fromString("b5f90002-1a2b-4c3d-8e9f-0a1b2c3d4e5f");
    static final UUID BOARD_STATUS = UUID.fromString("b5f90003-1a2b-4c3d-8e9f-0a1b2c3d4e5f");
    static final UUID BOARD_EVENT = UUID.fromString("b5f90004-1a2b-4c3d-8e9f-0a1b2c3d4e5f");
    static final String BOARD_NAME = "WT Board";
    static final UUID NUS = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
    static final UUID NUS_WRITE = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e");
    static final UUID NUS_NOTIFY = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e");
    static final String RD_NAME_PREFIX = "EDS OX";
    static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    static final long HOLD_MS = 540, REPEAT_MS = 300; // like the board's own buttons
    static final long HELLO_TIMEOUT_MS = 6000; // no session by then: the derailleur's app slot is taken, reconnect

    final Handler ui = new Handler(Looper.getMainLooper());
    SharedPreferences prefs;
    BluetoothLeScanner scanner;
    boolean scanning, permitted;

    TextView rdView, boardView, gearView, pairedView;
    Button upButton, downButton, pairButton;
    RadioGroup modeGroup;
    RadioButton modeApp, modePod;
    boolean updatingUi;

    // ---------- one GATT connection with its own operation queue ----------
    // Android allows one GATT operation at a time per connection, so they run from a queue.

    abstract class Link {
        BluetoothGatt gatt;
        final ArrayDeque<Runnable> ops = new ArrayDeque<>();
        boolean busy;

        boolean active() { return gatt != null; }

        void connect(BluetoothDevice d) {
            gatt = d.connectGatt(MainActivity.this, false, callback, BluetoothDevice.TRANSPORT_LE);
        }

        void close() {
            if (gatt != null) gatt.close();
            gatt = null;
            ops.clear();
            busy = false;
        }

        // Drop the connection and look for the device again.
        void restart() {
            if (gatt == null) return;
            gatt.disconnect();
            close();
            onClosed();
            render();
            ui.postDelayed(MainActivity.this::startScan, 1000);
        }

        void enqueue(Runnable op) {
            ops.add(op);
            if (!busy) next();
        }

        void next() {
            Runnable op = ops.poll();
            busy = op != null;
            if (op != null) op.run();
        }

        void write(BluetoothGattCharacteristic c, byte[] data, int type) {
            if (ops.size() > 3) return; // a held button must not pile up writes
            enqueue(() -> {
                boolean ok;
                if (Build.VERSION.SDK_INT >= 33) {
                    ok = gatt.writeCharacteristic(c, data, type) == BluetoothGatt.GATT_SUCCESS;
                } else {
                    c.setWriteType(type);
                    c.setValue(data);
                    ok = gatt.writeCharacteristic(c);
                }
                if (!ok) ui.post(this::next);
            });
        }

        void subscribe(BluetoothGattCharacteristic c) {
            gatt.setCharacteristicNotification(c, true);
            BluetoothGattDescriptor d = c.getDescriptor(CCCD);
            if (d == null) return;
            enqueue(() -> {
                boolean ok;
                if (Build.VERSION.SDK_INT >= 33) {
                    ok = gatt.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) == BluetoothGatt.GATT_SUCCESS;
                } else {
                    d.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
                    ok = gatt.writeDescriptor(d);
                }
                if (!ok) ui.post(this::next);
            });
        }

        void read(BluetoothGattCharacteristic c) {
            enqueue(() -> { if (!gatt.readCharacteristic(c)) ui.post(this::next); });
        }

        abstract void onServices(BluetoothGatt g);
        abstract void onValue(UUID chr, byte[] value);
        abstract void onClosed();

        final BluetoothGattCallback callback = new BluetoothGattCallback() {
            @Override
            public void onConnectionStateChange(BluetoothGatt g, int st, int state) {
                ui.post(() -> {
                    if (g != gatt) return;
                    if (state == BluetoothProfile.STATE_CONNECTED) {
                        // The board's status is longer than the default 20-byte notification.
                        if (!g.requestMtu(247)) g.discoverServices();
                    } else {
                        close();
                        onClosed();
                        render();
                        ui.postDelayed(MainActivity.this::startScan, 1000);
                    }
                });
            }

            @Override
            public void onMtuChanged(BluetoothGatt g, int mtu, int st) {
                ui.post(() -> { if (g == gatt) g.discoverServices(); });
            }

            @Override
            public void onServicesDiscovered(BluetoothGatt g, int st) {
                ui.post(() -> { if (g == gatt) { onServices(g); render(); } });
            }

            @Override
            public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int st) {
                ui.post(() -> { if (g == gatt) next(); });
            }

            @Override
            public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int st) {
                ui.post(() -> { if (g == gatt) next(); });
            }

            // Android 13 and newer
            @Override
            public void onCharacteristicRead(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] v, int st) {
                ui.post(() -> { if (g == gatt) { onValue(c.getUuid(), v); next(); } });
            }

            @Override
            public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] v) {
                ui.post(() -> { if (g == gatt) onValue(c.getUuid(), v); });
            }

            // Android 12 and older
            @Override
            @SuppressWarnings("deprecation")
            public void onCharacteristicRead(BluetoothGatt g, BluetoothGattCharacteristic c, int st) {
                byte[] v = c.getValue();
                ui.post(() -> { if (g == gatt) { onValue(c.getUuid(), v); next(); } });
            }

            @Override
            @SuppressWarnings("deprecation")
            public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
                byte[] v = c.getValue();
                ui.post(() -> { if (g == gatt) onValue(c.getUuid(), v); });
            }
        };
    }

    // ---------- the derailleur, like the Wheeltop app ----------
    // Frame: fe 32 <session> <command> <length> <data> <CRC-16/MODBUS, high byte first>.
    // Replies are XORed with the key (K − 0x32), K being their second byte.

    class RdLink extends Link {
        BluetoothGattCharacteristic rx;
        int session = -1; // from the RD's hello reply; the RD ignores commands with another session
        int gear = -1;

        final Runnable helloRetry = new Runnable() {
            @Override public void run() {
                if (rx == null || session >= 0) return;
                hello();
                ui.postDelayed(this, 2000);
            }
        };

        final Runnable helloTimeout = () -> { if (session < 0) restart(); };

        // Android may wait about 30 s for a derailleur that doesn't take the connection: give up sooner.
        @Override
        void connect(BluetoothDevice d) {
            super.connect(d);
            ui.postDelayed(helloTimeout, HELLO_TIMEOUT_MS);
        }

        @Override
        void onServices(BluetoothGatt g) {
            BluetoothGattService svc = g.getService(NUS);
            BluetoothGattCharacteristic notify = svc == null ? null : svc.getCharacteristic(NUS_NOTIFY);
            rx = svc == null ? null : svc.getCharacteristic(NUS_WRITE);
            if (rx == null || notify == null) {
                g.disconnect();
                return;
            }
            subscribe(notify);
            ui.post(helloRetry);
        }

        void hello() { send(0x00, 0x11, "yOTmK50z".getBytes(StandardCharsets.US_ASCII)); }

        void shift(boolean up) {
            if (session >= 0) send(session, 0x99, new byte[]{(byte) (up ? 0x02 : 0x01)});
        }

        void send(int s, int cmd, byte[] data) {
            byte[] f = new byte[7 + data.length];
            f[0] = (byte) 0xfe;
            f[1] = 0x32;
            f[2] = (byte) s;
            f[3] = (byte) cmd;
            f[4] = (byte) data.length;
            System.arraycopy(data, 0, f, 5, data.length);
            int c = crc16modbus(f, 5 + data.length);
            f[5 + data.length] = (byte) (c >> 8);
            f[6 + data.length] = (byte) c;
            write(rx, f, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE);
        }

        @Override
        void onValue(UUID chr, byte[] b) {
            if (b == null || b.length < 5 || (b[0] & 0xff) != 0xfe) return;
            int key = (b[1] - 0x32) & 0xff;
            byte[] q = new byte[b.length - 4];
            for (int i = 0; i < q.length; i++) q[i] = (byte) (b[2 + i] ^ key);
            if (q.length >= 2 && (q[1] & 0xff) == 0x11) {
                session = q[0] & 0xff;
                render();
            } else if (q.length >= 8 && (q[1] & 0xff) == 0x98) { // gear report S 98 06 4f 00 21 00 GG 00
                gear = q[7] & 0xff;
                render();
            }
        }

        @Override
        void onClosed() {
            ui.removeCallbacks(helloRetry);
            ui.removeCallbacks(helloTimeout);
            rx = null;
            session = -1;
            gear = -1;
        }
    }

    static int crc16modbus(byte[] p, int n) {
        int c = 0xffff;
        for (int i = 0; i < n; i++) {
            c ^= p[i] & 0xff;
            for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? (c >>> 1) ^ 0xa001 : c >>> 1;
        }
        return c;
    }

    // ---------- the board's settings ----------

    class BoardLink extends Link {
        BluetoothGattCharacteristic command, statusChr;
        Map<String, String> status = new HashMap<>();
        boolean wasYielded;

        @Override
        void onServices(BluetoothGatt g) {
            BluetoothGattService svc = g.getService(BOARD_SERVICE);
            BluetoothGattCharacteristic st = svc == null ? null : svc.getCharacteristic(BOARD_STATUS);
            command = svc == null ? null : svc.getCharacteristic(BOARD_COMMAND);
            if (st == null || command == null) {
                g.disconnect();
                return;
            }
            // Subscribing to events first tells the board to leave the derailleur to the phone.
            BluetoothGattCharacteristic ev = svc.getCharacteristic(BOARD_EVENT);
            if (ev != null) subscribe(ev);
            statusChr = st;
            subscribe(st);
            read(st);
        }

        void send(String cmd) {
            if (command != null) write(command, cmd.getBytes(StandardCharsets.UTF_8), BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
        }

        @Override
        void onValue(UUID chr, byte[] v) {
            if (v == null) return;
            if (BOARD_EVENT.equals(chr)) { // a board button
                if (v.length > 0) rd.shift(v[0] == 'u');
                return;
            }
            Map<String, String> s = new HashMap<>();
            for (String part : new String(v, StandardCharsets.UTF_8).split(";")) {
                int eq = part.indexOf('=');
                if (eq > 0) s.put(part.substring(0, eq), part.substring(eq + 1));
            }
            // A notification cut to the MTU lacks the last field: read the whole status.
            if (!s.containsKey("addr") && statusChr != null) {
                read(statusChr);
                return;
            }
            status = s;
            // Shift through the derailleur the board is paired with.
            String paired = s.get("paired");
            if (paired != null && paired.length() == 17) prefs.edit().putString("rd", paired.toUpperCase()).apply();
            // While pairing the board needs the derailleur itself. A connection made while the board still held
            // the derailleur gets no session: once the board lets go, start it again.
            boolean yielded = "1".equals(s.get("yield"));
            if (pairing() && rd.active()) rd.restart();
            else if (yielded && !wasYielded && rd.active() && rd.session < 0) rd.restart();
            wasYielded = yielded;
            startScan();
            render();
        }

        @Override
        void onClosed() {
            command = null;
            statusChr = null;
            status = new HashMap<>();
            wasYielded = false;
        }

        boolean ready() { return command != null && !status.isEmpty(); }
        boolean podMode() { return "pod".equals(status.get("mode")); }
        boolean pairing() { return "1".equals(status.get("pairing")); }

        // Whether the phone may connect to the derailleur: a board in phone mode must have let it go first.
        boolean rdFree() {
            if (!active()) return true;
            return ready() && !pairing() && (podMode() || "1".equals(status.get("yield")));
        }
    }

    final RdLink rd = new RdLink();
    final BoardLink board = new BoardLink();

    // ---------- activity ----------

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        prefs = getSharedPreferences("wtboard", MODE_PRIVATE);
        buildUi();
        render();
        String[] perms = Build.VERSION.SDK_INT >= 31
                ? new String[]{Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT}
                : new String[]{Manifest.permission.ACCESS_FINE_LOCATION};
        for (String p : perms)
            if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(perms, 1);
                return;
            }
        permitted = true;
        startScan();
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        for (int r : results)
            if (r != PackageManager.PERMISSION_GRANTED) {
                rdView.setText(R.string.need_permission);
                return;
            }
        permitted = true;
        startScan();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        ui.removeCallbacksAndMessages(null);
        stopScan();
        rd.close();
        board.close();
    }

    // ---------- UI: fits one screen, the shift buttons take the free space ----------

    int dp(int v) { return Math.round(v * getResources().getDisplayMetrics().density); }

    TextView text(LinearLayout root, int sizeSp) {
        TextView t = new TextView(this);
        t.setTextSize(sizeSp);
        root.addView(t);
        return t;
    }

    Button shiftButton(LinearLayout root, int res, boolean up) {
        Button b = new Button(this);
        b.setText(res);
        b.setTextSize(28);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1);
        lp.topMargin = dp(6);
        root.addView(b, lp);
        // A press shifts once; holding repeats every 0.3 s after 0.54 s.
        Runnable repeat = new Runnable() {
            @Override public void run() {
                rd.shift(up);
                ui.postDelayed(this, REPEAT_MS);
            }
        };
        b.setOnTouchListener((v, e) -> {
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    if (!v.isEnabled()) return false;
                    v.setPressed(true);
                    rd.shift(up);
                    ui.postDelayed(repeat, HOLD_MS);
                    return true;
                case MotionEvent.ACTION_UP:
                    v.performClick();
                    // fall through
                case MotionEvent.ACTION_CANCEL:
                    v.setPressed(false);
                    ui.removeCallbacks(repeat);
                    return true;
            }
            return false;
        });
        return b;
    }

    void buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(16), dp(12), dp(16), dp(12));
        setContentView(root);
        // Android 15 draws apps under the status and navigation bars: keep the content clear of them.
        if (Build.VERSION.SDK_INT >= 30) {
            root.setOnApplyWindowInsetsListener((v, insets) -> {
                android.graphics.Insets bars = insets.getInsets(android.view.WindowInsets.Type.systemBars());
                v.setPadding(dp(16) + bars.left, dp(12) + bars.top, dp(16) + bars.right, dp(12) + bars.bottom);
                return insets;
            });
        }

        rdView = text(root, 15);
        gearView = text(root, 30);
        gearView.setGravity(Gravity.CENTER_HORIZONTAL);
        gearView.setTypeface(Typeface.DEFAULT_BOLD);
        upButton = shiftButton(root, R.string.up, true);
        downButton = shiftButton(root, R.string.down, false);

        TextView title = text(root, 16);
        title.setText(R.string.mode_title);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setPadding(0, dp(12), 0, 0);
        boardView = text(root, 14);
        modeGroup = new RadioGroup(this);
        modeApp = new RadioButton(this);
        modeApp.setText(R.string.mode_app);
        modeApp.setId(View.generateViewId());
        modePod = new RadioButton(this);
        modePod.setText(R.string.mode_pod);
        modePod.setId(View.generateViewId());
        modeGroup.addView(modeApp);
        modeGroup.addView(modePod);
        root.addView(modeGroup);
        modeGroup.setOnCheckedChangeListener((g, id) -> {
            if (updatingUi) return;
            boolean toPod = id == modePod.getId();
            new AlertDialog.Builder(this)
                    .setMessage(toPod ? R.string.mode_confirm_pod : R.string.mode_confirm_app)
                    .setPositiveButton(R.string.ok, (d, w) -> board.send(toPod ? "mode pod" : "mode app"))
                    .setNegativeButton(R.string.cancel, (d, w) -> render())
                    .setOnCancelListener(d -> render())
                    .show();
        });

        pairButton = new Button(this);
        pairButton.setText(R.string.pair_button);
        root.addView(pairButton);
        pairButton.setOnClickListener(v -> {
            if (board.podMode()) {
                // In shifter mode a button press during the derailleur's pairing window pairs the board.
                AlertDialog d = new AlertDialog.Builder(this)
                        .setMessage(R.string.pair_pod_help)
                        .setPositiveButton(R.string.pair_pod_button, null)
                        .setNegativeButton(R.string.cancel, null)
                        .show();
                d.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(b -> board.send("u"));
            } else {
                new AlertDialog.Builder(this)
                        .setMessage(R.string.pair_app_confirm)
                        .setPositiveButton(R.string.pair, (d, w) -> board.send("pair"))
                        .setNegativeButton(R.string.cancel, null)
                        .show();
            }
        });
        pairedView = text(root, 13);
    }

    void render() {
        if (permitted) {
            rdView.setText(rd.session >= 0 ? R.string.rd_connected
                    : rd.active() ? R.string.rd_connecting : R.string.rd_searching);
        }
        gearView.setText(rd.gear >= 0 ? getString(R.string.gear, String.valueOf(rd.gear)) : "");
        upButton.setEnabled(rd.session >= 0);
        downButton.setEnabled(rd.session >= 0);

        boolean b = board.ready();
        boardView.setText(!b ? R.string.board_searching
                : board.pairing() ? R.string.board_pairing
                : "1".equals(board.status.get("yield")) ? R.string.board_yielded : R.string.board_connected);
        for (View v : new View[]{modeApp, modePod, pairButton}) v.setEnabled(b);
        String paired = board.status.get("paired");
        pairedView.setText(b && paired != null && !paired.equals("-") ? getString(R.string.paired_with, paired) : "");
        updatingUi = true;
        if (!b) modeGroup.clearCheck();
        else modeGroup.check(board.podMode() ? modePod.getId() : modeApp.getId());
        updatingUi = false;
    }

    // ---------- scanning for the derailleur and the board ----------

    void startScan() {
        BluetoothManager bm = getSystemService(BluetoothManager.class);
        BluetoothAdapter adapter = bm == null ? null : bm.getAdapter();
        if (adapter == null || !adapter.isEnabled()) {
            rdView.setText(R.string.bluetooth_off);
            ui.postDelayed(this::startScan, 2000);
            return;
        }
        if (scanning || (rd.active() && board.active())) return;
        render();
        scanner = adapter.getBluetoothLeScanner();
        ScanSettings settings = new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();
        scanner.startScan(null, settings, scanCallback);
        scanning = true;
    }

    void stopScan() {
        if (scanning && scanner != null) scanner.stopScan(scanCallback);
        scanning = false;
    }

    final ScanCallback scanCallback = new ScanCallback() {
        @Override
        public void onScanResult(int type, ScanResult r) {
            ScanRecord rec = r.getScanRecord();
            if (rec == null) return;
            String name = rec.getDeviceName();
            BluetoothDevice d = r.getDevice();
            if (!rd.active() && name != null && name.startsWith(RD_NAME_PREFIX)) {
                if (!board.rdFree()) return;
                // The derailleur the board is paired with, or else the first one found.
                String known = prefs.getString("rd", null);
                if (known != null && !known.equalsIgnoreCase(d.getAddress())) return;
                prefs.edit().putString("rd", d.getAddress()).apply();
                rd.connect(d);
            } else if (!board.active() && (BOARD_NAME.equals(name)
                    || (rec.getServiceUuids() != null && rec.getServiceUuids().contains(new ParcelUuid(BOARD_SERVICE))))) {
                board.connect(d);
            } else {
                return;
            }
            if (rd.active() && board.active()) stopScan();
            render();
        }
    };
}

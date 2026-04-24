package com.xiaohei.fishlight;

import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.TextUtils;
import android.widget.Button;
import android.widget.EditText;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;
import androidx.core.content.ContextCompat;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.Date;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

public class MainActivity extends AppCompatActivity {

    private static final String PREFS = "xiaohei_aquarium_app";
    private static final String KEY_MODE = "mode";
    private static final String KEY_LOCAL_URL = "local_url";
    private static final String MODE_CLOUD = "CLOUD";
    private static final String MODE_LOCAL = "LOCAL";
    private static final int MAX_LOG_LINES = 14;
    private static final long LOCAL_POLL_MS = 3500L;
    private static final long CLOUD_REFRESH_TIMEOUT_MS = 4500L;
    private static final int HTTP_TIMEOUT_MS = 3500;

    private final Handler uiHandler = new Handler(Looper.getMainLooper());
    private final ExecutorService networkExecutor = Executors.newFixedThreadPool(2);
    private final ArrayDeque<String> logs = new ArrayDeque<>();
    private final AtomicBoolean localFetchInFlight = new AtomicBoolean(false);
    private final AtomicBoolean cloudQueryInFlight = new AtomicBoolean(false);

    private final Runnable localPollRunnable = new Runnable() {
        @Override
        public void run() {
            if (isLocalMode()) {
                fetchLocalStateAsync(false, false);
            }
            uiHandler.postDelayed(this, LOCAL_POLL_MS);
        }
    };

    private final Runnable cloudRefreshTimeoutRunnable = () -> {
        if (cloudQueryInFlight.getAndSet(false)) {
            appendLog("云端刷新等待超时，当前显示最近一次收到的状态。");
            refreshUi();
        }
    };

    private TextView tvModeStatus;
    private TextView tvCloudStatus;
    private TextView tvDeviceStatus;
    private TextView tvLightStatus;
    private TextView tvPumpStatus;
    private TextView tvMasterStatus;
    private TextView tvOverallStatus;
    private TextView tvSource;
    private TextView tvTopicRoot;
    private TextView tvLog;
    private EditText etLocalUrl;
    private RadioGroup rgMode;
    private RadioButton rbCloud;
    private RadioButton rbLocal;
    private Button btnRefresh;
    private Button btnLight;
    private Button btnPump;

    private CloudMqttClient cloudClient;
    private boolean brokerConnected = false;
    private boolean cloudDeviceOnline = false;
    private boolean localReachable = false;
    private boolean pendingCloudRefreshAfterConnect = false;

    private final DeviceState state = new DeviceState();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        bindViews();
        loadPrefs();
        setupCloudClient();
        setupActions();

        tvTopicRoot.setText("主题: " + CloudMqttClient.TOPIC_ROOT);
        appendLog("APP 已启动，等待设备状态同步。");
        refreshUi();
    }

    @Override
    protected void onResume() {
        super.onResume();
        cloudClient.start();
        uiHandler.removeCallbacks(localPollRunnable);
        uiHandler.post(localPollRunnable);
        queryByCurrentMode(false);
    }

    @Override
    protected void onPause() {
        super.onPause();
        uiHandler.removeCallbacks(localPollRunnable);
        uiHandler.removeCallbacks(cloudRefreshTimeoutRunnable);
        cloudClient.stop();
        savePrefs();
    }

    @Override
    protected void onDestroy() {
        cloudClient.stop();
        uiHandler.removeCallbacksAndMessages(null);
        networkExecutor.shutdownNow();
        super.onDestroy();
    }

    private void bindViews() {
        tvModeStatus = findViewById(R.id.tvModeStatus);
        tvCloudStatus = findViewById(R.id.tvCloudStatus);
        tvDeviceStatus = findViewById(R.id.tvDeviceStatus);
        tvLightStatus = findViewById(R.id.tvLightStatus);
        tvPumpStatus = findViewById(R.id.tvPumpStatus);
        tvMasterStatus = findViewById(R.id.tvMasterStatus);
        tvOverallStatus = findViewById(R.id.tvOverallStatus);
        tvSource = findViewById(R.id.tvSource);
        tvTopicRoot = findViewById(R.id.tvTopicRoot);
        tvLog = findViewById(R.id.tvLog);
        etLocalUrl = findViewById(R.id.etLocalUrl);
        rgMode = findViewById(R.id.rgMode);
        rbCloud = findViewById(R.id.rbCloud);
        rbLocal = findViewById(R.id.rbLocal);
        btnRefresh = findViewById(R.id.btnRefresh);
        btnLight = findViewById(R.id.btnLight);
        btnPump = findViewById(R.id.btnPump);
    }

    private void setupCloudClient() {
        cloudClient = new CloudMqttClient(new CloudMqttClient.Listener() {
            @Override
            public void onConnected() {
                brokerConnected = true;
                appendLog("云端 Broker 已连接。");
                refreshUi();

                if (!isLocalMode() || pendingCloudRefreshAfterConnect) {
                    pendingCloudRefreshAfterConnect = false;
                    queryCloudState(false);
                }
            }

            @Override
            public void onDisconnected(String reason) {
                brokerConnected = false;
                cloudDeviceOnline = false;
                uiHandler.removeCallbacks(cloudRefreshTimeoutRunnable);
                cloudQueryInFlight.set(false);
                appendLog("云端连接断开: " + reason);
                refreshUi();
            }

            @Override
            public void onStateMessage(String payload) {
                if (cloudQueryInFlight.getAndSet(false)) {
                    uiHandler.removeCallbacks(cloudRefreshTimeoutRunnable);
                    appendLog("云端状态已刷新。");
                }

                try {
                    JSONObject object = new JSONObject(payload);
                    applyStateFromJson(object, "cloud");
                } catch (JSONException e) {
                    appendLog("云端状态解析失败: " + e.getMessage());
                }
            }

            @Override
            public void onAvailabilityMessage(String payload) {
                String normalized = payload == null ? "" : payload.trim().toLowerCase(Locale.ROOT);
                cloudDeviceOnline = "online".equals(normalized);
                refreshUi();
            }
        });
    }

    private void setupActions() {
        rgMode.setOnCheckedChangeListener((group, checkedId) -> {
            refreshUi();
            savePrefs();
            queryByCurrentMode(false);
        });

        btnRefresh.setOnClickListener(v -> queryByCurrentMode(true));

        btnLight.setOnClickListener(v -> {
            String action = state.known ? (state.light ? "OFF" : "ON") : "TOGGLE";
            sendCommand("LIGHT", action);
        });

        btnPump.setOnClickListener(v -> {
            String action = state.known ? (state.pump ? "OFF" : "ON") : "TOGGLE";
            sendCommand("PUMP", action);
        });
    }

    private void queryByCurrentMode(boolean manual) {
        if (isLocalMode()) {
            fetchLocalStateAsync(true, manual);
        } else {
            queryCloudState(manual);
        }
    }

    private void queryCloudState(boolean manual) {
        if (!brokerConnected) {
            pendingCloudRefreshAfterConnect = true;
            if (manual) {
                appendLog("云端未连接，正在尝试重连并刷新。");
            }
            cloudClient.stop();
            cloudClient.start();
            refreshUi();
            return;
        }

        if (!cloudQueryInFlight.compareAndSet(false, true)) {
            if (manual) {
                appendLog("云端刷新进行中，请稍候。");
            }
            refreshUi();
            return;
        }

        uiHandler.removeCallbacks(cloudRefreshTimeoutRunnable);
        uiHandler.postDelayed(cloudRefreshTimeoutRunnable, CLOUD_REFRESH_TIMEOUT_MS);

        networkExecutor.execute(() -> {
            try {
                boolean ok = cloudClient.publishCommand("QUERY", "QUERY");
                uiHandler.post(() -> {
                    if (ok) {
                        if (manual) {
                            appendLog("已发送云端刷新请求。");
                        }
                    } else {
                        uiHandler.removeCallbacks(cloudRefreshTimeoutRunnable);
                        cloudQueryInFlight.set(false);
                        appendLog("云端暂不可用，刷新请求未发送。");
                    }
                    refreshUi();
                });
            } catch (Exception e) {
                uiHandler.post(() -> {
                    uiHandler.removeCallbacks(cloudRefreshTimeoutRunnable);
                    cloudQueryInFlight.set(false);
                    appendLog("云端刷新失败: " + safeMessage(e));
                    refreshUi();
                });
            }
        });
    }

    private void sendCommand(String target, String action) {
        if (isLocalMode()) {
            sendLocalCommandAsync(target, action);
        } else {
            sendCloudCommandAsync(target, action);
        }
    }

    private void sendCloudCommandAsync(String target, String action) {
        networkExecutor.execute(() -> {
            try {
                boolean ok = cloudClient.publishCommand(target, action);
                uiHandler.post(() -> {
                    if (ok) {
                        appendLog("云端指令已发送: " + target + " -> " + action);
                    } else {
                        appendLog("云端未连接，指令未发送。");
                    }
                    refreshUi();
                });
            } catch (Exception e) {
                uiHandler.post(() -> appendLog("云端发送失败: " + safeMessage(e)));
            }
        });
    }

    private void sendLocalCommandAsync(String target, String action) {
        final String baseUrl = getLocalBaseUrl();
        if (TextUtils.isEmpty(baseUrl)) {
            appendLog("本地地址为空，请输入 ESP 地址。");
            return;
        }

        networkExecutor.execute(() -> {
            try {
                String url = baseUrl + "/api/control?target="
                    + URLEncoder.encode(target, StandardCharsets.UTF_8.name())
                    + "&action="
                    + URLEncoder.encode(action, StandardCharsets.UTF_8.name());
                HttpResult result = httpGet(url);
                uiHandler.post(() -> {
                    if (result.ok) {
                        localReachable = true;
                        appendLog("本地下发成功: " + target + " -> " + action);
                        refreshUi();
                        uiHandler.postDelayed(() -> fetchLocalStateAsync(false, false), 500);
                    } else {
                        localReachable = false;
                        appendLog("本地下发失败: " + result.error);
                        refreshUi();
                    }
                });
            } catch (Exception e) {
                uiHandler.post(() -> {
                    localReachable = false;
                    appendLog("本地下发异常: " + safeMessage(e));
                    refreshUi();
                });
            }
        });
    }

    private void fetchLocalStateAsync(boolean logWhenSuccess, boolean manual) {
        final String baseUrl = getLocalBaseUrl();
        if (TextUtils.isEmpty(baseUrl)) {
            if (manual || logWhenSuccess) {
                appendLog("请先填写本地 ESP 地址。");
            }
            localReachable = false;
            refreshUi();
            return;
        }

        if (!localFetchInFlight.compareAndSet(false, true)) {
            if (manual) {
                appendLog("本地刷新进行中，请稍候。");
            }
            refreshUi();
            return;
        }

        networkExecutor.execute(() -> {
            try {
                HttpResult result = httpGet(baseUrl + "/api/state");
                if (!result.ok) {
                    throw new RuntimeException(result.error);
                }
                JSONObject object = new JSONObject(result.body);
                uiHandler.post(() -> {
                    localReachable = true;
                    applyStateFromJson(object, "local");
                    if (manual || logWhenSuccess) {
                        appendLog("本地状态已刷新。");
                    }
                    localFetchInFlight.set(false);
                    refreshUi();
                });
            } catch (Exception e) {
                uiHandler.post(() -> {
                    localReachable = false;
                    if (manual || logWhenSuccess) {
                        appendLog("本地状态获取失败: " + safeMessage(e));
                    }
                    localFetchInFlight.set(false);
                    refreshUi();
                });
            }
        });
    }

    private void applyStateFromJson(JSONObject object, String channel) {
        state.known = object.optBoolean("known", true);
        state.light = object.optBoolean("light", false);
        state.pump = object.optBoolean("pump", false);
        state.masterSwitch = object.optBoolean("masterSwitch", false);
        state.overall = object.optBoolean("overall", state.light || state.pump);
        state.source = object.optString("source", "UNKNOWN");

        if (!"local".equals(channel)) {
            cloudDeviceOnline = true;
        }

        savePrefs();
        refreshUi();
    }

    private HttpResult httpGet(String urlText) {
        HttpURLConnection connection = null;
        try {
            URL url = new URL(urlText);
            connection = (HttpURLConnection) url.openConnection();
            connection.setRequestMethod("GET");
            connection.setConnectTimeout(HTTP_TIMEOUT_MS);
            connection.setReadTimeout(HTTP_TIMEOUT_MS);
            connection.connect();

            int code = connection.getResponseCode();
            InputStream stream = code >= 200 && code < 300
                ? connection.getInputStream()
                : connection.getErrorStream();
            String body = readAll(stream);
            if (code >= 200 && code < 300) {
                return HttpResult.success(body);
            }
            return HttpResult.error("HTTP " + code + " " + body);
        } catch (Exception e) {
            return HttpResult.error(safeMessage(e));
        } finally {
            if (connection != null) {
                connection.disconnect();
            }
        }
    }

    private String readAll(InputStream stream) {
        if (stream == null) {
            return "";
        }
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(stream, StandardCharsets.UTF_8))) {
            StringBuilder builder = new StringBuilder();
            String line;
            while ((line = reader.readLine()) != null) {
                builder.append(line);
            }
            return builder.toString();
        } catch (Exception e) {
            return "";
        }
    }

    private boolean isLocalMode() {
        return rgMode.getCheckedRadioButtonId() == R.id.rbLocal;
    }

    private String getLocalBaseUrl() {
        String raw = etLocalUrl.getText() == null ? "" : etLocalUrl.getText().toString().trim();
        if (raw.isEmpty()) {
            return "";
        }
        if (!raw.startsWith("http://") && !raw.startsWith("https://")) {
            raw = "http://" + raw;
        }
        while (raw.endsWith("/")) {
            raw = raw.substring(0, raw.length() - 1);
        }
        return raw;
    }

    private void refreshUi() {
        String modeText = isLocalMode() ? "当前模式：ESP 本地网络" : "当前模式：云端 MQTT";
        tvModeStatus.setText(modeText);

        String cloudText = brokerConnected
            ? "云端已连接 (" + cloudClient.getActiveTransportHost() + ":" + cloudClient.getActiveTransportPort() + ")"
            : "云端未连接";
        tvCloudStatus.setText(cloudText);

        boolean channelOnline = isLocalMode() ? localReachable : (brokerConnected && cloudDeviceOnline);
        tvDeviceStatus.setText(channelOnline ? "设备在线" : "设备离线");
        tvDeviceStatus.setTextColor(color(channelOnline ? R.color.xh_online : R.color.xh_offline));

        if (!state.known) {
            tvLightStatus.setText("鱼缸照明：未知");
            tvPumpStatus.setText("鱼泵状态：未知");
            tvMasterStatus.setText("总开关：未知");
            tvOverallStatus.setText("设备总状态：未知");
            tvSource.setText("最后来源：未知");
        } else {
            tvLightStatus.setText("鱼缸照明：" + (state.light ? "打开" : "关闭"));
            tvPumpStatus.setText("鱼泵状态：" + (state.pump ? "打开" : "关闭"));
            tvMasterStatus.setText("总开关：" + (state.masterSwitch ? "打开" : "关闭"));
            tvOverallStatus.setText("设备总状态：" + (state.overall ? "运行中" : "已停止"));
            tvSource.setText("最后来源：" + state.source);
        }

        btnLight.setText(state.known && state.light ? "关闭照明" : "打开照明");
        btnPump.setText(state.known && state.pump ? "关闭鱼泵" : "打开鱼泵");
        updateRefreshButtonState();
        applyControlButtonStyles(channelOnline);
        renderLogs();
    }

    private void updateRefreshButtonState() {
        boolean refreshing = isLocalMode()
            ? localFetchInFlight.get()
            : (cloudQueryInFlight.get() || pendingCloudRefreshAfterConnect);
        btnRefresh.setEnabled(!refreshing);
        btnRefresh.setText(refreshing ? "刷新中..." : "刷新状态");
        btnRefresh.setAlpha(refreshing ? 0.8f : 1.0f);
    }

    private void applyControlButtonStyles(boolean channelOnline) {
        boolean canControl = isLocalMode()
            ? localReachable
            : (brokerConnected && cloudDeviceOnline);

        btnLight.setEnabled(canControl);
        btnPump.setEnabled(canControl);

        int lightColor;
        int pumpColor;
        if (!canControl) {
            lightColor = color(R.color.xh_control_disabled);
            pumpColor = color(R.color.xh_control_disabled);
        } else {
            lightColor = color(state.known && state.light ? R.color.xh_control_on : R.color.xh_control_off);
            pumpColor = color(state.known && state.pump ? R.color.xh_control_on : R.color.xh_control_off);
        }

        btnLight.setBackgroundTintList(ColorStateList.valueOf(lightColor));
        btnPump.setBackgroundTintList(ColorStateList.valueOf(pumpColor));
        btnLight.setTextColor(color(android.R.color.white));
        btnPump.setTextColor(color(android.R.color.white));
        float alpha = channelOnline ? 1.0f : 0.92f;
        btnLight.setAlpha(alpha);
        btnPump.setAlpha(alpha);
    }

    private void appendLog(String text) {
        String time = new SimpleDateFormat("HH:mm:ss", Locale.CHINA).format(new Date());
        logs.addLast(time + "  " + text);
        while (logs.size() > MAX_LOG_LINES) {
            logs.removeFirst();
        }
        renderLogs();
    }

    private void renderLogs() {
        if (logs.isEmpty()) {
            tvLog.setText("等待日志...");
            return;
        }
        StringBuilder builder = new StringBuilder();
        for (String line : logs) {
            if (builder.length() > 0) {
                builder.append('\n');
            }
            builder.append(line);
        }
        tvLog.setText(builder.toString());
    }

    private void loadPrefs() {
        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        String mode = prefs.getString(KEY_MODE, MODE_CLOUD);
        String localUrl = prefs.getString(KEY_LOCAL_URL, "http://192.168.3.26");
        etLocalUrl.setText(localUrl);
        if (MODE_LOCAL.equals(mode)) {
            rbLocal.setChecked(true);
        } else {
            rbCloud.setChecked(true);
        }
    }

    private void savePrefs() {
        String mode = isLocalMode() ? MODE_LOCAL : MODE_CLOUD;
        getSharedPreferences(PREFS, MODE_PRIVATE)
            .edit()
            .putString(KEY_MODE, mode)
            .putString(KEY_LOCAL_URL, getLocalBaseUrl())
            .apply();
    }

    private String safeMessage(Exception e) {
        if (e == null || TextUtils.isEmpty(e.getMessage())) {
            return "未知错误";
        }
        return e.getMessage();
    }

    private int color(int colorResId) {
        return ContextCompat.getColor(this, colorResId);
    }

    private static final class DeviceState {
        boolean known = false;
        boolean light = false;
        boolean pump = false;
        boolean masterSwitch = false;
        boolean overall = false;
        String source = "UNKNOWN";
    }

    private static final class HttpResult {
        final boolean ok;
        final String body;
        final String error;

        private HttpResult(boolean ok, String body, String error) {
            this.ok = ok;
            this.body = body;
            this.error = error;
        }

        static HttpResult success(String body) {
            return new HttpResult(true, body, "");
        }

        static HttpResult error(String error) {
            return new HttpResult(false, "", error);
        }
    }
}

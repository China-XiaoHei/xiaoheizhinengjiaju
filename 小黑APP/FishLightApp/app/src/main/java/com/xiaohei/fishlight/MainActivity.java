package com.xiaohei.fishlight;

import android.content.SharedPreferences;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.TextUtils;
import android.widget.Button;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;

import org.json.JSONException;
import org.json.JSONObject;

import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.Date;
import java.util.Locale;

public class MainActivity extends AppCompatActivity {

    private static final String PREFS_NAME = "fishlight_state";
    private static final String KEY_POWER_KNOWN = "power_known";
    private static final String KEY_POWER_ON = "power_on";
    private static final String KEY_LAST_SOURCE = "last_source";
    private static final long COMMAND_TIMEOUT_MS = 4000L;
    private static final int MAX_LOG_LINES = 14;

    private final Handler uiHandler = new Handler(Looper.getMainLooper());
    private final ArrayDeque<String> logs = new ArrayDeque<>();
    private final Runnable commandTimeoutRunnable = () -> {
        commandPending = false;
        appendLog("等待设备回传状态超时，请检查设备是否在线。");
        refreshUi();
    };

    private TextView tvCloudBadge;
    private TextView tvDeviceBadge;
    private TextView tvPowerBadge;
    private TextView tvStatusDetail;
    private TextView tvLastSource;
    private TextView tvLog;
    private TextView tvTopicRoot;
    private Button btnToggle;

    private CloudMqttClient cloudClient;
    private boolean brokerConnected = false;
    private boolean deviceOnline = false;
    private boolean powerKnown = false;
    private boolean powerOn = false;
    private boolean commandPending = false;
    private String lastSource = "sync";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        tvCloudBadge = findViewById(R.id.tvCloudBadge);
        tvDeviceBadge = findViewById(R.id.tvDeviceBadge);
        tvPowerBadge = findViewById(R.id.tvPowerBadge);
        tvStatusDetail = findViewById(R.id.tvStatusDetail);
        tvLastSource = findViewById(R.id.tvLastSource);
        tvLog = findViewById(R.id.tvLog);
        tvTopicRoot = findViewById(R.id.tvTopicRoot);
        btnToggle = findViewById(R.id.btnToggle);

        tvTopicRoot.setText(CloudMqttClient.TOPIC_ROOT);
        loadCachedState();
        setupCloudClient();

        btnToggle.setOnClickListener(v -> sendToggleCommand());
        appendLog("APP 已启动，准备连接云端。");
        refreshUi();
    }

    @Override
    protected void onResume() {
        super.onResume();
        cloudClient.start();
    }

    @Override
    protected void onPause() {
        super.onPause();
        uiHandler.removeCallbacks(commandTimeoutRunnable);
        commandPending = false;
        if (cloudClient != null) {
            cloudClient.stop();
        }
    }

    @Override
    protected void onDestroy() {
        uiHandler.removeCallbacksAndMessages(null);
        if (cloudClient != null) {
            cloudClient.stop();
        }
        super.onDestroy();
    }

    private void setupCloudClient() {
        cloudClient = new CloudMqttClient(new CloudMqttClient.Listener() {
            @Override
            public void onConnected() {
                brokerConnected = true;
                appendLog("云端 Broker 已连接，正在查询最新灯状态。");
                refreshUi();
                requestLatestState();
            }

            @Override
            public void onDisconnected(String reason) {
                boolean wasOnline = brokerConnected || deviceOnline;
                brokerConnected = false;
                deviceOnline = false;
                commandPending = false;
                uiHandler.removeCallbacks(commandTimeoutRunnable);
                if (wasOnline) {
                    appendLog("云端连接断开：" + reason);
                }
                refreshUi();
            }

            @Override
            public void onStateMessage(String payload) {
                applyStatePayload(payload);
            }

            @Override
            public void onAvailabilityMessage(String payload) {
                applyAvailabilityPayload(payload);
            }
        });
    }

    private void requestLatestState() {
        try {
            if (cloudClient.publishCommand("QUERY")) {
                appendLog("已向设备请求最新状态。");
            }
        } catch (Exception e) {
            appendLog("请求状态失败：" + safeMessage(e));
        }
    }

    private void sendToggleCommand() {
        if (!brokerConnected || !deviceOnline || !powerKnown || commandPending) {
            return;
        }

        String command = powerOn ? "OFF" : "ON";
        String actionText = powerOn ? "关灯" : "开灯";
        try {
            if (!cloudClient.publishCommand(command)) {
                appendLog("命令发送失败，云端暂未连通。");
                refreshUi();
                return;
            }
            commandPending = true;
            uiHandler.removeCallbacks(commandTimeoutRunnable);
            uiHandler.postDelayed(commandTimeoutRunnable, COMMAND_TIMEOUT_MS);
            appendLog("已发送指令：" + actionText);
            refreshUi();
        } catch (Exception e) {
            appendLog("命令发送失败：" + safeMessage(e));
            refreshUi();
        }
    }

    private void applyAvailabilityPayload(String payload) {
        String normalized = payload == null ? "" : payload.trim().toLowerCase(Locale.ROOT);
        boolean nextOnline = "online".equals(normalized);
        if (deviceOnline != nextOnline) {
            deviceOnline = nextOnline;
            if (nextOnline) {
                appendLog("设备已上线，可以远程控制鱼缸照明。");
                requestLatestState();
            } else {
                appendLog("设备已离线，等待重新上线。");
            }
        } else {
            deviceOnline = nextOnline;
        }
        refreshUi();
    }

    private void applyStatePayload(String payload) {
        boolean previousKnown = powerKnown;
        boolean previousPower = powerOn;
        String previousSource = lastSource;

        if (!parseStatePayload(payload)) {
            appendLog("收到无法识别的状态消息：" + payload);
            return;
        }

        brokerConnected = true;
        deviceOnline = true;
        commandPending = false;
        uiHandler.removeCallbacks(commandTimeoutRunnable);
        saveCachedState();

        boolean changed = !previousKnown || previousPower != powerOn || !TextUtils.equals(previousSource, lastSource);
        if (changed) {
            appendLog("状态同步：" + CloudMqttClient.DEVICE_NAME + (powerOn ? "已开启" : "已关闭")
                + "，来源：" + sourceToChinese(lastSource) + "。");
        }
        refreshUi();
    }

    private boolean parseStatePayload(String payload) {
        if (TextUtils.isEmpty(payload)) {
            return false;
        }

        String trimmed = payload.trim();
        if (trimmed.startsWith("{")) {
            try {
                JSONObject object = new JSONObject(trimmed);
                if (!object.has("power")) {
                    return false;
                }
                powerOn = object.optBoolean("power", false);
                powerKnown = true;
                lastSource = object.optString("source", "sync");
                return true;
            } catch (JSONException e) {
                return false;
            }
        }

        String normalized = trimmed.toUpperCase(Locale.ROOT);
        if ("ON".equals(normalized)) {
            powerOn = true;
            powerKnown = true;
            lastSource = "sync";
            return true;
        }
        if ("OFF".equals(normalized)) {
            powerOn = false;
            powerKnown = true;
            lastSource = "sync";
            return true;
        }
        return false;
    }

    private void refreshUi() {
        tvCloudBadge.setText(brokerConnected
            ? (cloudClient.isUsingTls() ? "云端已连接 TLS" : "云端已连接")
            : "云端连接中");
        tvCloudBadge.setBackgroundResource(brokerConnected ? R.drawable.bg_status_online : R.drawable.bg_status_waiting);

        tvDeviceBadge.setText(deviceOnline ? "设备在线" : "设备离线");
        tvDeviceBadge.setBackgroundResource(deviceOnline ? R.drawable.bg_status_online : R.drawable.bg_status_offline);

        if (powerKnown) {
            tvPowerBadge.setText(powerOn ? "照明已开启" : "照明已关闭");
            tvPowerBadge.setBackgroundResource(powerOn ? R.drawable.bg_status_power_on : R.drawable.bg_status_power_off);
        } else {
            tvPowerBadge.setText("等待状态同步");
            tvPowerBadge.setBackgroundResource(R.drawable.bg_status_waiting);
        }

        String brokerText;
        if (brokerConnected) {
            String transport = cloudClient.isUsingTls() ? "TLS " + cloudClient.getActiveTransportPort() : String.valueOf(cloudClient.getActiveTransportPort());
            brokerText = "Broker：" + CloudMqttClient.DEFAULT_HOST + " : " + transport;
        } else {
            brokerText = "Broker：正在重连 " + CloudMqttClient.DEFAULT_HOST;
        }

        String deviceText;
        if (deviceOnline) {
            deviceText = "设备在线，本地物理按钮和 APP 会自动保持同步。";
        } else if (brokerConnected) {
            deviceText = "Broker 已连接，但设备暂未上线。";
        } else {
            deviceText = "正在连接云端，请稍候。";
        }

        String lampText = powerKnown
            ? CloudMqttClient.DEVICE_NAME + "当前状态：" + (powerOn ? "开启" : "关闭")
            : CloudMqttClient.DEVICE_NAME + "当前状态：等待同步";
        tvStatusDetail.setText(brokerText + "\n" + deviceText + "\n" + lampText);
        tvLastSource.setText("最后来源：" + sourceToChinese(lastSource));

        boolean canToggle = brokerConnected && deviceOnline && powerKnown && !commandPending;
        btnToggle.setEnabled(canToggle);
        btnToggle.setAlpha(canToggle ? 1.0f : 0.55f);
        if (commandPending) {
            btnToggle.setText("等待同步...");
        } else if (!powerKnown) {
            btnToggle.setText("等待同步");
        } else {
            btnToggle.setText(powerOn ? "关灯" : "开灯");
        }

        renderLogs();
    }

    private void loadCachedState() {
        SharedPreferences preferences = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        powerKnown = preferences.getBoolean(KEY_POWER_KNOWN, false);
        powerOn = preferences.getBoolean(KEY_POWER_ON, false);
        lastSource = preferences.getString(KEY_LAST_SOURCE, "sync");
    }

    private void saveCachedState() {
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
            .edit()
            .putBoolean(KEY_POWER_KNOWN, powerKnown)
            .putBoolean(KEY_POWER_ON, powerOn)
            .putString(KEY_LAST_SOURCE, lastSource)
            .apply();
    }

    private void appendLog(String message) {
        String time = new SimpleDateFormat("HH:mm:ss", Locale.CHINA).format(new Date());
        logs.addLast(time + "  " + message);
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

    private String sourceToChinese(String source) {
        if ("button".equalsIgnoreCase(source)) {
            return "本地按钮";
        }
        if ("cloud".equalsIgnoreCase(source)) {
            return "APP / 云端";
        }
        if ("boot".equalsIgnoreCase(source)) {
            return "设备上电";
        }
        if ("sync".equalsIgnoreCase(source)) {
            return "状态同步";
        }
        return TextUtils.isEmpty(source) ? "未知" : source;
    }

    private String safeMessage(Exception exception) {
        if (exception == null || TextUtils.isEmpty(exception.getMessage())) {
            return "未知错误";
        }
        return exception.getMessage();
    }
}

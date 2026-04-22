package com.xiaohei.fishlight;

import android.os.Handler;
import android.os.Looper;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

import javax.net.SocketFactory;
import javax.net.ssl.SSLSocketFactory;

public final class CloudMqttClient {

    public interface Listener {
        void onConnected();
        void onDisconnected(String reason);
        void onStateMessage(String payload);
        void onAvailabilityMessage(String payload);
    }

    public static final String DEVICE_NAME = "鱼缸照明";
    public static final String DEFAULT_HOST = "broker-cn.emqx.io";
    public static final String DEFAULT_HOST_BACKUP = "broker.emqx.io";
    public static final int DEFAULT_PORT = 1883;
    public static final int DEFAULT_TLS_PORT = 8883;

    public static final String TOPIC_ROOT = "xiaohei/aquarium/khome-20260422";
    public static final String TOPIC_STATE = TOPIC_ROOT + "/state";
    public static final String TOPIC_AVAILABILITY = TOPIC_ROOT + "/availability";
    public static final String TOPIC_CMD_LIGHT = TOPIC_ROOT + "/cmd/light";
    public static final String TOPIC_CMD_PUMP = TOPIC_ROOT + "/cmd/pump";
    public static final String TOPIC_CMD_MASTER = TOPIC_ROOT + "/cmd/master";
    public static final String TOPIC_CMD_QUERY = TOPIC_ROOT + "/cmd/query";

    private static final int CONNECT_TIMEOUT_MS = 3500;
    private static final int SOCKET_TIMEOUT_MS = 1000;
    private static final int KEEP_ALIVE_SEC = 45;
    private static final long PING_INTERVAL_MS = 15000L;
    private static final long PING_TIMEOUT_MS = 5000L;
    private static final long RECONNECT_DELAY_MS = 3000L;

    private final Listener listener;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final Object sendLock = new Object();

    private volatile Socket socket;
    private volatile InputStream inputStream;
    private volatile OutputStream outputStream;
    private volatile boolean connected = false;
    private volatile Thread workerThread;
    private volatile int generation = 0;
    private volatile int packetId = 1;
    private volatile long lastTxMs = 0L;
    private volatile long lastRxMs = 0L;
    private volatile long lastPingMs = 0L;
    private volatile boolean waitingPingResp = false;
    private volatile String activeTransportHost = DEFAULT_HOST;
    private volatile int activeTransportPort = DEFAULT_PORT;
    private volatile boolean usingTls = false;

    public CloudMqttClient(Listener listener) {
        this.listener = listener;
    }

    public synchronized void start() {
        if (workerThread != null && workerThread.isAlive()) {
            return;
        }

        generation++;
        connected = false;
        closeSocketQuietly();

        final int workerGeneration = generation;
        workerThread = new Thread(() -> runLoop(workerGeneration), "xiaohei-aquarium-mqtt");
        workerThread.start();
    }

    public synchronized void stop() {
        generation++;
        connected = false;
        closeSocketQuietly();
        Thread thread = workerThread;
        workerThread = null;
        if (thread != null) {
            thread.interrupt();
        }
    }

    public boolean isConnected() {
        return connected;
    }

    public boolean isUsingTls() {
        return usingTls;
    }

    public int getActiveTransportPort() {
        return activeTransportPort;
    }

    public String getActiveTransportHost() {
        return activeTransportHost;
    }

    public boolean publishCommand(String target, String action) throws IOException {
        if (target == null || action == null) {
            return false;
        }

        String topic;
        String cleanTarget = target.trim().toUpperCase(Locale.ROOT);
        String cleanAction = action.trim().toUpperCase(Locale.ROOT);

        if ("QUERY".equals(cleanTarget)) {
            topic = TOPIC_CMD_QUERY;
            cleanAction = "QUERY";
        } else if ("LIGHT".equals(cleanTarget)) {
            topic = TOPIC_CMD_LIGHT;
        } else if ("PUMP".equals(cleanTarget)) {
            topic = TOPIC_CMD_PUMP;
        } else if ("MASTER".equals(cleanTarget) || "ALL".equals(cleanTarget)) {
            topic = TOPIC_CMD_MASTER;
        } else {
            return false;
        }

        return sendPublish(topic, cleanAction, false);
    }

    private boolean isWorkerActive(int workerGeneration) {
        return workerGeneration == generation && Thread.currentThread() == workerThread;
    }

    private void runLoop(int workerGeneration) {
        while (isWorkerActive(workerGeneration)) {
            try {
                connectAndHandshake(workerGeneration);
                notifyConnected();
                readLoop(workerGeneration);
            } catch (Exception e) {
                notifyDisconnected(e.getMessage() == null ? "cloud disconnected" : e.getMessage());
            } finally {
                connected = false;
                waitingPingResp = false;
                closeSocketQuietly();
            }

            if (!isWorkerActive(workerGeneration)) {
                break;
            }

            try {
                Thread.sleep(RECONNECT_DELAY_MS);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
                break;
            }
        }
    }

    private void connectAndHandshake(int workerGeneration) throws IOException {
        ConnectionTarget target = connectWithFallback();
        Socket nextSocket = target.socket;
        nextSocket.setSoTimeout(SOCKET_TIMEOUT_MS);

        socket = nextSocket;
        inputStream = nextSocket.getInputStream();
        outputStream = nextSocket.getOutputStream();
        activeTransportHost = target.host;
        activeTransportPort = target.port;
        usingTls = target.tls;
        packetId = 1;
        lastTxMs = System.currentTimeMillis();
        lastRxMs = lastTxMs;
        waitingPingResp = false;

        sendPacket(buildConnectPacket(buildClientId()));
        byte[] connAck = readPacketBlocking(workerGeneration, 5000L);
        if (connAck == null || connAck.length < 4 || (connAck[0] & 0xF0) != 0x20 || connAck[3] != 0x00) {
            throw new IOException("mqtt connack failed");
        }

        sendPacket(buildSubscribePacket(new String[]{TOPIC_STATE, TOPIC_AVAILABILITY}));
        byte[] subAck = readPacketBlocking(workerGeneration, 5000L);
        if (subAck == null || subAck.length < 5 || (subAck[0] & 0xF0) != 0x90) {
            throw new IOException("mqtt suback failed");
        }

        if (!isWorkerActive(workerGeneration)) {
            throw new IOException("mqtt worker switched");
        }
    }

    private ConnectionTarget connectWithFallback() throws IOException {
        IOException lastError = null;

        ConnectionTarget[] targets = buildConnectionTargets();

        for (ConnectionTarget target : targets) {
            try {
                SocketFactory factory = target.tls ? SSLSocketFactory.getDefault() : SocketFactory.getDefault();
                Socket nextSocket = factory.createSocket();
                nextSocket.connect(new InetSocketAddress(target.host, target.port), CONNECT_TIMEOUT_MS);
                target.socket = nextSocket;
                return target;
            } catch (IOException e) {
                lastError = e;
            }
        }

        if (lastError != null) {
            throw lastError;
        }
        throw new IOException("broker connect failed");
    }

    private ConnectionTarget[] buildConnectionTargets() {
        if (DEFAULT_HOST.equalsIgnoreCase(DEFAULT_HOST_BACKUP)) {
            return new ConnectionTarget[]{
                new ConnectionTarget(DEFAULT_HOST, DEFAULT_PORT, false),
                new ConnectionTarget(DEFAULT_HOST, DEFAULT_TLS_PORT, true)
            };
        }
        return new ConnectionTarget[]{
            new ConnectionTarget(DEFAULT_HOST, DEFAULT_PORT, false),
            new ConnectionTarget(DEFAULT_HOST, DEFAULT_TLS_PORT, true),
            new ConnectionTarget(DEFAULT_HOST_BACKUP, DEFAULT_PORT, false),
            new ConnectionTarget(DEFAULT_HOST_BACKUP, DEFAULT_TLS_PORT, true)
        };
    }

    private void readLoop(int workerGeneration) throws IOException {
        connected = true;
        while (isWorkerActive(workerGeneration)) {
            long now = System.currentTimeMillis();
            if (!waitingPingResp && now - lastTxMs >= PING_INTERVAL_MS && now - lastRxMs >= PING_INTERVAL_MS) {
                sendPacket(new byte[]{(byte) 0xC0, 0x00});
                waitingPingResp = true;
                lastPingMs = now;
            }
            if (waitingPingResp && now - lastPingMs >= PING_TIMEOUT_MS) {
                throw new IOException("broker ping timeout");
            }

            byte[] packet = readPacketOnce();
            if (packet != null) {
                handlePacket(packet);
            }
        }
    }

    private byte[] readPacketBlocking(int workerGeneration, long timeoutMs) throws IOException {
        long deadline = System.currentTimeMillis() + timeoutMs;
        while (isWorkerActive(workerGeneration) && System.currentTimeMillis() < deadline) {
            byte[] packet = readPacketOnce();
            if (packet != null) {
                return packet;
            }
        }
        return null;
    }

    private byte[] readPacketOnce() throws IOException {
        InputStream in = inputStream;
        if (in == null) {
            return null;
        }

        try {
            int header = in.read();
            if (header < 0) {
                throw new IOException("broker closed");
            }

            byte[] lengthBytes = new byte[4];
            int remainingLength = 0;
            int multiplier = 1;
            int lengthCount = 0;
            while (true) {
                int value = in.read();
                if (value < 0) {
                    throw new IOException("mqtt read length failed");
                }
                lengthBytes[lengthCount++] = (byte) value;
                remainingLength += (value & 0x7F) * multiplier;
                if ((value & 0x80) == 0) {
                    break;
                }
                multiplier *= 128;
                if (lengthCount >= 4) {
                    throw new IOException("invalid mqtt remaining length");
                }
            }

            byte[] packet = new byte[1 + lengthCount + remainingLength];
            packet[0] = (byte) header;
            System.arraycopy(lengthBytes, 0, packet, 1, lengthCount);
            readFully(in, packet, 1 + lengthCount, remainingLength);
            lastRxMs = System.currentTimeMillis();
            return packet;
        } catch (SocketTimeoutException ignored) {
            return null;
        }
    }

    private void handlePacket(byte[] packet) {
        int packetType = packet[0] & 0xF0;
        if (packetType == 0x30) {
            handlePublish(packet);
            return;
        }
        if (packetType == 0xD0) {
            waitingPingResp = false;
        }
    }

    private void handlePublish(byte[] packet) {
        int[] lengthInfo = decodeRemainingLength(packet, 1);
        int bodyStart = 1 + lengthInfo[1];
        if (packet.length < bodyStart + 2) {
            return;
        }

        int topicLength = ((packet[bodyStart] & 0xFF) << 8) | (packet[bodyStart + 1] & 0xFF);
        int topicStart = bodyStart + 2;
        int payloadStart = topicStart + topicLength;
        if (packet.length < payloadStart) {
            return;
        }

        int qos = (packet[0] >> 1) & 0x03;
        if (qos > 0) {
            payloadStart += 2;
        }
        if (packet.length < payloadStart) {
            return;
        }

        String topic = new String(packet, topicStart, topicLength, StandardCharsets.UTF_8);
        String payload = new String(packet, payloadStart, packet.length - payloadStart, StandardCharsets.UTF_8);
        if (TOPIC_STATE.equals(topic)) {
            mainHandler.post(() -> listener.onStateMessage(payload));
        } else if (TOPIC_AVAILABILITY.equals(topic)) {
            mainHandler.post(() -> listener.onAvailabilityMessage(payload));
        }
    }

    private boolean sendPublish(String topic, String payload, boolean retain) throws IOException {
        synchronized (sendLock) {
            if (!connected || outputStream == null) {
                return false;
            }
            sendPacket(buildPublishPacket(topic, payload, retain));
            return true;
        }
    }

    private void sendPacket(byte[] packet) throws IOException {
        OutputStream out = outputStream;
        if (out == null) {
            throw new IOException("mqtt output unavailable");
        }
        synchronized (sendLock) {
            out.write(packet);
            out.flush();
        }
        lastTxMs = System.currentTimeMillis();
    }

    private byte[] buildConnectPacket(String clientId) {
        byte[] payload = encodeString(clientId);
        byte[] variableHeader = new byte[]{
            0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x02,
            (byte) ((KEEP_ALIVE_SEC >> 8) & 0xFF),
            (byte) (KEEP_ALIVE_SEC & 0xFF)
        };
        return buildPacket((byte) 0x10, variableHeader, payload);
    }

    private byte[] buildSubscribePacket(String[] topics) {
        byte[] variableHeader = new byte[]{
            (byte) ((packetId >> 8) & 0xFF),
            (byte) (packetId & 0xFF)
        };
        packetId++;

        int payloadLength = 0;
        for (String topic : topics) {
            payloadLength += 2 + topic.getBytes(StandardCharsets.UTF_8).length + 1;
        }

        byte[] payload = new byte[payloadLength];
        int offset = 0;
        for (String topic : topics) {
            byte[] topicBytes = encodeString(topic);
            System.arraycopy(topicBytes, 0, payload, offset, topicBytes.length);
            offset += topicBytes.length;
            payload[offset++] = 0x00;
        }

        return buildPacket((byte) 0x82, variableHeader, payload);
    }

    private byte[] buildPublishPacket(String topic, String payload, boolean retain) {
        byte[] topicBytes = encodeString(topic);
        byte[] payloadBytes = payload.getBytes(StandardCharsets.UTF_8);
        byte header = (byte) (0x30 | (retain ? 0x01 : 0x00));
        return buildPacket(header, topicBytes, payloadBytes);
    }

    private byte[] buildPacket(byte header, byte[] variableHeader, byte[] payload) {
        int remainingLength = variableHeader.length + payload.length;
        byte[] remainingBytes = encodeRemainingLength(remainingLength);
        byte[] packet = new byte[1 + remainingBytes.length + remainingLength];
        int offset = 0;
        packet[offset++] = header;
        System.arraycopy(remainingBytes, 0, packet, offset, remainingBytes.length);
        offset += remainingBytes.length;
        System.arraycopy(variableHeader, 0, packet, offset, variableHeader.length);
        offset += variableHeader.length;
        System.arraycopy(payload, 0, packet, offset, payload.length);
        return packet;
    }

    private byte[] encodeString(String value) {
        byte[] textBytes = value.getBytes(StandardCharsets.UTF_8);
        byte[] encoded = new byte[textBytes.length + 2];
        encoded[0] = (byte) ((textBytes.length >> 8) & 0xFF);
        encoded[1] = (byte) (textBytes.length & 0xFF);
        System.arraycopy(textBytes, 0, encoded, 2, textBytes.length);
        return encoded;
    }

    private byte[] encodeRemainingLength(int value) {
        byte[] temp = new byte[4];
        int count = 0;
        int remaining = value;
        do {
            int encoded = remaining % 128;
            remaining /= 128;
            if (remaining > 0) {
                encoded |= 0x80;
            }
            temp[count++] = (byte) encoded;
        } while (remaining > 0 && count < temp.length);

        byte[] result = new byte[count];
        System.arraycopy(temp, 0, result, 0, count);
        return result;
    }

    private int[] decodeRemainingLength(byte[] packet, int offset) {
        int multiplier = 1;
        int value = 0;
        int count = 0;
        int index = offset;
        while (index < packet.length) {
            int encoded = packet[index] & 0xFF;
            value += (encoded & 0x7F) * multiplier;
            multiplier *= 128;
            count++;
            index++;
            if ((encoded & 0x80) == 0) {
                break;
            }
        }
        return new int[]{value, count};
    }

    private void readFully(InputStream in, byte[] buffer, int offset, int length) throws IOException {
        int readTotal = 0;
        while (readTotal < length) {
            int read = in.read(buffer, offset + readTotal, length - readTotal);
            if (read < 0) {
                throw new IOException("mqtt read interrupted");
            }
            readTotal += read;
        }
    }

    private String buildClientId() {
        long suffix = System.currentTimeMillis() & 0xFFFFFFL;
        return String.format(Locale.US, "xiaohei_aquarium_%06X", suffix);
    }

    private void notifyConnected() {
        mainHandler.post(listener::onConnected);
    }

    private void notifyDisconnected(String reason) {
        final String message = (reason == null || reason.trim().isEmpty()) ? "cloud disconnected" : reason;
        mainHandler.post(() -> listener.onDisconnected(message));
    }

    private void closeSocketQuietly() {
        try {
            if (inputStream != null) {
                inputStream.close();
            }
        } catch (IOException ignored) {
        }
        try {
            if (outputStream != null) {
                outputStream.close();
            }
        } catch (IOException ignored) {
        }
        try {
            if (socket != null) {
                socket.close();
            }
        } catch (IOException ignored) {
        }
        inputStream = null;
        outputStream = null;
        socket = null;
    }

    private static final class ConnectionTarget {
        final String host;
        final int port;
        final boolean tls;
        Socket socket;

        ConnectionTarget(String host, int port, boolean tls) {
            this.host = host;
            this.port = port;
            this.tls = tls;
        }
    }
}

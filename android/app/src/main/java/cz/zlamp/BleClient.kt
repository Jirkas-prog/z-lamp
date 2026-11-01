@file:Suppress("DEPRECATION")
package cz.zlamp

import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.*
import android.os.*
import org.json.JSONObject
import java.util.UUID

/** All GATT operations and callbacks are serialized on the main looper. */
@SuppressLint("MissingPermission") // MainActivity gates scan/connect on runtime permissions.
class BleClient(private val context: Context) {
    companion object {
        val SERVICE: UUID = UUID.fromString("bcf83ad0-e398-4d7b-9d6c-31dbe2a2a001")
        val RX: UUID = UUID.fromString("bcf83ad0-e398-4d7b-9d6c-31dbe2a2a003")
        val TX: UUID = UUID.fromString("bcf83ad0-e398-4d7b-9d6c-31dbe2a2a004")
        val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }
    private val handler = Handler(Looper.getMainLooper())
    private val adapter = context.getSystemService(BluetoothManager::class.java)?.adapter
    private val prefs = context.getSharedPreferences("lamp", Context.MODE_PRIVATE)
    private var gatt: BluetoothGatt? = null
    private var bondingDevice: BluetoothDevice? = null
    private var rx: BluetoothGattCharacteristic? = null
    private var mtu = 23
    private var phase = "idle"
    private var sequence = 0
    private var scan: ScanCallback? = null
    private val queue = Requests()
    private val receiver = Wire.Receiver()
    private var current: Requests.Job? = null
    private var frames = emptyList<ByteArray>()
    private var writeIndex = 0
    private var earlyReply: JSONObject? = null
    private var timer: Runnable? = null
    private var scanTimer: Runnable? = null
    private var lastSend = 0L
    private var pumping = false
    private var connectionGeneration = 0
    private var closed = false
    var ready = false; private set
    var status = "Nepřipojeno"; private set
    var info = JSONObject(); private set
    var onStatus: ((String) -> Unit)? = null
    var onReady: (() -> Unit)? = null
    var onChanged: (() -> Unit)? = null
    var onLink: ((Boolean) -> Unit)? = null
    var onSignal: ((Int?) -> Unit)? = null
    var lastRssi: Int? = null; private set
    var rssiAt = 0L; private set
    var connectionAddress = ""; private set
    private var automatic = false
    private var rssiPending = false
    private val rssiTimeout = Runnable { if (rssiPending) fail("Měření signálu vypršelo; obnovuji spojení.") }
    val savedAddress: String get() = prefs.getString("address", "") ?: ""
    val savedName: String get() = prefs.getString("name", "Z-lamp") ?: "Z-lamp"
    val available get() = adapter != null
    val bluetoothEnabled get() = adapter?.isEnabled == true
    val scanning get() = scan != null
    val connecting get() = phase != "idle" && phase != "ready"
    val pairing get() = phase == "bond"
    val autoConnecting get() = automatic && connecting

    private fun message(text: String) { status = text; onStatus?.invoke(text) }
    private fun arm(milliseconds: Long, text: String) {
        timer?.let(handler::removeCallbacks)
        timer = Runnable { fail(text) }.also { handler.postDelayed(it, milliseconds) }
    }
    private fun disarm() { timer?.let(handler::removeCallbacks); timer = null }
    private fun run(g: BluetoothGatt, body: () -> Unit) {
        handler.post { if (!closed && gatt === g) try { body() } catch (e: Exception) { fail(e.message ?: "Chyba Bluetooth.") } }
    }
    private val bondReceiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, intent: Intent) {
            val device = if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
                else intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
            val pending = bondingDevice ?: return
            try {
                if (device?.address != pending.address || phase != "bond") return
                // The event carries its own state; the BluetoothDevice cache can lag it.
                when (intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.ERROR)) {
                    BluetoothDevice.BOND_BONDED -> bonded(device)
                    BluetoothDevice.BOND_NONE -> fail("Párování se nezdařilo. Vyber lampu znovu a přijmi případné potvrzení Androidu.")
                }
            } catch (_: SecurityException) { fail("Povol Bluetooth v nastavení aplikace.") }
        }
    }
    private val bondCheck = object : Runnable {
        override fun run() {
            val device = bondingDevice ?: return
            if (phase != "bond") return
            try {
                if (device.bondState == BluetoothDevice.BOND_BONDED) bonded(device)
                else handler.postDelayed(this, 1000)
            } catch (_: SecurityException) { fail("Povol Bluetooth v nastavení aplikace.") }
        }
    }
    init {
        val filter = IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED)
        if (Build.VERSION.SDK_INT >= 33) context.registerReceiver(bondReceiver, filter, Context.RECEIVER_EXPORTED)
        else context.registerReceiver(bondReceiver, filter)
    }
    fun scan(found: (BluetoothDevice, String, Int) -> Unit) {
        stopScan()
        if (!bluetoothEnabled) { message("Zapni Bluetooth v telefonu."); return }
        val scanner = adapter?.bluetoothLeScanner ?: return
        val callback = object : ScanCallback() {
            override fun onScanResult(type: Int, result: ScanResult) {
                handler.post { if (scan === this) try {
                    found(result.device, result.scanRecord?.deviceName ?: result.device.name ?: "Z-lamp", result.rssi)
                } catch (_: SecurityException) { stopScan(); message("Povol Bluetooth v nastavení aplikace.") } }
            }
            override fun onScanFailed(code: Int) { handler.post { if (scan === this) { stopScan(); message("Hledání selhalo ($code). Počkej chvíli a zkus znovu.") } } }
        }
        scan = callback
        scanner.startScan(listOf(ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE)).build()),
            ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), callback)
        scanTimer = Runnable { stopScan(); if (!ready) message("Hledání dokončeno. Vyber lampu, nebo hledej znovu.") }.also { handler.postDelayed(it, 12000) }
        message("Hledám lampy v okolí…")
    }
    fun stopScan() {
        scanTimer?.let(handler::removeCallbacks); scanTimer = null
        val old = scan; scan = null
        try { if (old != null) adapter?.bluetoothLeScanner?.stopScan(old) } catch (_: SecurityException) { }
    }
    fun isBonded(address: String): Boolean = try {
        adapter?.getRemoteDevice(address)?.bondState == BluetoothDevice.BOND_BONDED
    } catch (_: Exception) { false }
    fun connect(address: String, autoConnect: Boolean = false) {
        if (address == connectionAddress && (ready || connecting)) return
        disconnect("Připojuji…")
        automatic = autoConnect; connectionAddress = address
        if (!bluetoothEnabled) { message("Zapni Bluetooth v telefonu."); return }
        try {
            val device = adapter!!.getRemoteDevice(address)
            // Explicit selection always connects; the gateway starts Just Works bonding.
            if (automatic && device.bondState != BluetoothDevice.BOND_BONDED) fail("Nejdřív lampu znovu spáruj v aplikaci.")
            else openGatt(device)
        } catch (e: Exception) { fail(e.message ?: "Nelze připojit Bluetooth.") }
    }
    private fun bonded(device: BluetoothDevice) {
        bondingDevice = null; handler.removeCallbacks(bondCheck); disarm()
        val active = gatt
        if (active == null) openGatt(device) else negotiate(active)
    }
    private fun openGatt(device: BluetoothDevice) {
        phase = "connect"; message(if (automatic) "Čekám na uloženou lampu v dosahu…" else "Připojuji lampu…")
        gatt = device.connectGatt(context, automatic, callback, BluetoothDevice.TRANSPORT_LE, BluetoothDevice.PHY_LE_1M_MASK, handler)
        if (gatt == null) fail("Telefon nemohl otevřít BLE spojení.")
        else if (!automatic) arm(20000, "Lampa neodpovídá. Ověř její napájení a zkus připojení znovu.")
    }
    private fun negotiate(g: BluetoothGatt) {
        if (phase != "connect" && phase != "bond") return
        phase = "mtu"; message("Nastavuji zabezpečené spojení…")
        arm(12000, "Nastavení Bluetooth vypršelo. Připoj lampu znovu.")
        if (!g.requestMtu(247)) discover(g)
    }
    private fun discover(g: BluetoothGatt) {
        phase = "services"; arm(12000, "Lampa neposkytla ovládací službu.")
        if (!g.discoverServices()) fail("Nelze načíst služby lampy.")
    }
    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, code: Int, state: Int) = run(g) {
            if (code != BluetoothGatt.GATT_SUCCESS || state == BluetoothProfile.STATE_DISCONNECTED) {
                // Keep Android's native autoConnect registration when the peer leaves range.
                if (automatic && phase != "bond" && code in listOf(0, 8, 19, 22, 62) && isBonded(g.device.address)) {
                    resetLink("Spojení přerušeno ($code) · čekám na návrat lampy…", keepGatt = true)
                    return@run
                }
                val stage = when (phase) { "connect" -> "připojování"; "bond" -> "párování"; "ready" -> "ovládání"; else -> "nastavování spojení" }
                fail("Spojení přerušeno při $stage${if (code != 0) " ($code)" else ""}. Neodeslané změny byly zrušeny; po připojení se načte stav.")
            } else if (state == BluetoothProfile.STATE_CONNECTED) {
                if (phase != "connect") return@run
                if (g.device.bondState == BluetoothDevice.BOND_BONDED) negotiate(g)
                else if (automatic) fail("Obnov párování lampy v aplikaci.")
                else {
                    bondingDevice = g.device; phase = "bond"
                    message("Páruji bez PINu… Pokud Android zobrazí potvrzení, přijmi ho.")
                    arm(30000, "Párování se nedokončilo. Vyber lampu znovu; zkontroluj i oznámení Androidu.")
                    handler.postDelayed(bondCheck, 1000)
                }
            }
        }
        override fun onReadRemoteRssi(g: BluetoothGatt, rssi: Int, status: Int) = run(g) {
            if (!rssiPending || !ready) return@run
            rssiPending = false; handler.removeCallbacks(rssiTimeout)
            lastRssi = rssi.takeIf { status == BluetoothGatt.GATT_SUCCESS && it in -120..-1 }
            rssiAt = SystemClock.elapsedRealtime(); onSignal?.invoke(lastRssi); pump()
        }
        override fun onMtuChanged(g: BluetoothGatt, value: Int, status: Int) = run(g) {
            if (phase == "mtu") { mtu = if (status == BluetoothGatt.GATT_SUCCESS) value.coerceIn(23, 247) else 23; discover(g) }
        }
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) = run(g) {
            if (phase != "services") return@run
            require(status == BluetoothGatt.GATT_SUCCESS) { "Načtení služeb selhalo ($status)." }
            val service = g.getService(SERVICE) ?: error("Tato lampa nemá firmware Wi-Fi + BLE 2.8.0.")
            rx = service.getCharacteristic(RX) ?: error("Chybí ovládací kanál.")
            val tx = service.getCharacteristic(TX) ?: error("Chybí odpovědi lampy.")
            require(rx!!.properties and BluetoothGattCharacteristic.PROPERTY_WRITE != 0 && tx.properties and BluetoothGattCharacteristic.PROPERTY_INDICATE != 0)
            val descriptor = tx.getDescriptor(CCCD) ?: error("Chybí odběr odpovědí.")
            require(g.setCharacteristicNotification(tx, true)) { "Nelze zapnout odpovědi." }
            phase = "subscribe"; arm(12000, "Lampa nepotvrdila odběr odpovědí.")
            val ok = if (Build.VERSION.SDK_INT >= 33) g.writeDescriptor(descriptor, BluetoothGattDescriptor.ENABLE_INDICATION_VALUE) == BluetoothStatusCodes.SUCCESS
                else { descriptor.value = BluetoothGattDescriptor.ENABLE_INDICATION_VALUE; g.writeDescriptor(descriptor) }
            require(ok) { "Nelze nastavit odpovědi lampy." }
        }
        override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) = run(g) {
            if (phase != "subscribe" || descriptor.uuid != CCCD) return@run
            require(status == BluetoothGatt.GATT_SUCCESS) { "Párování nebo odběr odpovědí selhal ($status)." }
            disarm(); phase = "info"
            enqueue("get_info", JSONObject()) { result ->
                result.onSuccess { data ->
                    if (gatt !== g) return@onSuccess
                    if (data.optInt("v") != 1 || data.optInt("maxMessage") != Wire.MAX) { fail("Nepodporovaná verze protokolu lampy."); return@onSuccess }
                    info = data; phase = "ready"; ready = true
                    prefs.edit().putString("address", g.device.address).putString("name", data.optString("alias", "Z-lamp")).apply()
                    message("Připojeno · ${data.optString("alias")} · BLE"); onLink?.invoke(true); onReady?.invoke()
                }.onFailure { if (gatt === g && phase == "info") fail(it.message ?: "Chyba protokolu.") }
            }
        }
        override fun onCharacteristicWrite(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) = run(g) {
            if (characteristic.uuid != RX || current == null) return@run
            require(status == BluetoothGatt.GATT_SUCCESS) { "Lampa odmítla zápis ($status). Výsledek příkazu není známý; znovu se připoj." }
            writeIndex++
            if (writeIndex < frames.size) writeFrame(g)
            else earlyReply?.let(::complete)
        }
        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray) {
            if (characteristic.uuid == TX) received(g, value.copyOf())
        }
        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            if (Build.VERSION.SDK_INT < 33 && characteristic.uuid == TX) received(g, characteristic.value.copyOf())
        }
    }
    private fun received(g: BluetoothGatt, bytes: ByteArray) = run(g) {
        if (phase != "info" && phase != "ready") return@run
        val text = receiver.feed(bytes, SystemClock.elapsedRealtime()) ?: return@run
        val response = JSONObject(text)
        require(response.optInt("v") == 1) { "Neznámá BLE odpověď." }
        if (response.optString("event") == "changed") { if (ready) onChanged?.invoke(); return@run }
        require(current != null && response.optInt("id") == sequence && response.has("ok")) { "Odpověď nepatří aktuálnímu příkazu." }
        if (writeIndex < frames.size) earlyReply = response else complete(response)
    }
    fun request(op: String, args: JSONObject = JSONObject(), reply: (Result<JSONObject>) -> Unit) {
        if (!ready) { reply(Result.failure(IllegalStateException("Lampa není připojená."))); return }
        // Starting/stopping a scene supersedes slider changes still waiting to be sent.
        if (op == "scene/run" || op == "scene/stop") queue.discardControls()
        enqueue(op, args, reply)
    }
    private fun enqueue(op: String, args: JSONObject, reply: (Result<JSONObject>) -> Unit) {
        if (!queue.add(op, args, reply)) reply(Result.failure(IllegalStateException("Příliš mnoho požadavků. Počkej na dokončení.")))
        pump()
    }
    fun control(target: String, patch: JSONObject, reply: (Result<JSONObject>) -> Unit) {
        if (!ready) { reply(Result.failure(IllegalStateException("Lampa není připojená."))); return }
        if (!queue.control(target, patch, reply)) reply(Result.failure(IllegalStateException("Fronta změn je plná.")))
        pump()
    }
    /** RSSI shares the same GATT lane as commands; stale callbacks die with the connection. */
    fun readSignal() {
        if (!ready || current != null || pumping || queue.size > 0 || rssiPending) return
        try {
            rssiPending = gatt?.readRemoteRssi() == true
            if (rssiPending) handler.postDelayed(rssiTimeout, 6000)
            else { lastRssi = null; onSignal?.invoke(null) }
        } catch (_: SecurityException) { fail("Povol Bluetooth v nastavení aplikace.") }
    }
    private fun pump() {
        if (current != null || rssiPending || pumping || gatt == null || (phase != "ready" && phase != "info")) return
        pumping = true
        val generation = connectionGeneration
        handler.postDelayed({
            if (generation != connectionGeneration) return@postDelayed
            pumping = false
            if (current != null || rssiPending || gatt == null || (phase != "ready" && phase != "info")) return@postDelayed
            val job = queue.take() ?: return@postDelayed
            sequence = if (sequence == Int.MAX_VALUE) 1 else sequence+1
            try {
                frames = Wire.frames(JSONObject().put("v", 1).put("id", sequence).put("op", job.op).put("args", job.args).toString(), mtu)
            } catch (e: Exception) { job.reply(Result.failure(e)); pump(); return@postDelayed }
            current = job; earlyReply = null; writeIndex = 0; lastSend = SystemClock.elapsedRealtime()
            arm(15000, "Odpověď vypršela. Příkaz mohl proběhnout; po připojení načti stav a neopakuj ho naslepo.")
            try { writeFrame(gatt!!) } catch (e: Exception) { fail(e.message ?: "Chyba přenosu.") }
        }, maxOf(0, 110-(SystemClock.elapsedRealtime()-lastSend)))
    }
    private fun writeFrame(g: BluetoothGatt) {
        val characteristic = rx ?: error("Chybí ovládací kanál.")
        val bytes = frames[writeIndex]
        val ok = if (Build.VERSION.SDK_INT >= 33) g.writeCharacteristic(characteristic, bytes, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothStatusCodes.SUCCESS
            else { characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT; characteristic.value = bytes; g.writeCharacteristic(characteristic) }
        require(ok) { "Bluetooth je zaneprázdněné. Spojení bylo ukončeno bez opakování příkazu." }
    }
    private fun complete(response: JSONObject) {
        val job = current ?: return
        current = null; earlyReply = null; frames = emptyList(); disarm()
        if (response.getBoolean("ok")) job.reply(Result.success(response.optJSONObject("data") ?: JSONObject()))
        else job.reply(Result.failure(IllegalStateException(friendly(response.optString("error", "unknown")))))
        pump()
    }
    private fun fail(reason: String) = disconnect(reason)
    fun disconnect(reason: String = "Odpojeno") = resetLink(reason, keepGatt = false)
    private fun resetLink(reason: String, keepGatt: Boolean) {
        val wasReady = ready
        rssiPending = false; handler.removeCallbacks(rssiTimeout); lastRssi = null; rssiAt = 0L
        stopScan(); disarm(); handler.removeCallbacks(bondCheck); ready = false; phase = if (keepGatt) "connect" else "idle"; bondingDevice = null; receiver.reset(); rx = null; mtu = 23
        connectionGeneration++; pumping = false
        if (!keepGatt) {
            val previous = gatt; gatt = null
            try { previous?.disconnect(); previous?.close() } catch (_: SecurityException) { }
        }
        val job = current; current = null; frames = emptyList(); earlyReply = null
        queue.clear(reason); job?.reply?.invoke(Result.failure(IllegalStateException(reason)))
        if (wasReady) onLink?.invoke(false)
        message(reason)
    }
    fun forgetSaved() { disconnect(); prefs.edit().clear().apply() }
    fun close() { closed = true; onStatus = null; onReady = null; onChanged = null; onSignal = null; disconnect(); onLink = null; context.unregisterReceiver(bondReceiver) }
    private fun friendly(error: String): String = when (error) {
        "invalid_timer" -> "Zadej platnou délku časovače, nejvýše 24 hodin."
        "timer_requires_bulb" -> "Vyber alespoň jednu Zigbee žárovku. Displej má vlastní uspávání."
        "coordinator_update_required" -> "Aktualizuj C6 na firmware 2.7.1 nebo novější."
        "arrival_requires_bulb" -> "Noční časovač vyžaduje jednu konkrétní Zigbee žárovku."
        "arrival_storage_failed" -> "Nepodařilo se bezpečně uložit časovač v lampě."
        "coordinator_offline" -> "Zigbee deska neodpovídá. Zkontroluj napájení a UART."
        "coordinator_rejected_or_timeout" -> "Zigbee deska příkaz nepotvrdila. Zkontroluj stav světla."
        "update_busy" -> "Probíhá aktualizace. Počkej na dokončení."
        "unknown_light", "unknown_group", "unknown_scene" -> "Tato položka už v lampě neexistuje. Obnov seznam."
        "invalid_name" -> "Název musí mít 1–32 bajtů UTF-8."
        "empty_group", "empty_scene" -> "Vyber alespoň jedno světlo."
        "invalid_scene" -> "Zkontroluj barvy, délku přechodu a výběr světel."
        "group_limit", "scene_limit" -> "Dosažen limit osmi položek."
        else -> "Lampa příkaz odmítla: $error"
    }
}

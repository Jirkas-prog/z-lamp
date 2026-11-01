@file:Suppress("DEPRECATION")
package cz.zlamp

import android.Manifest
import android.app.*
import android.bluetooth.BluetoothAdapter
import android.companion.AssociationRequest
import android.companion.BluetoothDeviceFilter
import android.companion.CompanionDeviceManager
import android.content.*
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.content.res.ColorStateList
import android.location.LocationManager
import android.net.Uri
import android.os.*
import android.provider.Settings
import android.text.InputType
import android.text.SpannableString
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.view.*
import android.widget.*
import org.json.JSONArray
import org.json.JSONObject
import kotlin.math.roundToInt

class MainActivity : Activity() {
    private val ink = Color.rgb(241, 246, 233)
    private val muted = Color.rgb(159, 178, 174)
    private val mint = Color.rgb(215, 250, 120)
    private val panelColor = Color.rgb(19, 24, 27)
    private val backgroundColor = Color.rgb(9, 13, 15)
    private val prefs by lazy { getSharedPreferences("appearance", MODE_PRIVATE) }
    private val handler = Handler(Looper.getMainLooper())
    private val app get() = application as LampApp
    private var arrivalDialog: AlertDialog? = null
    private lateinit var client: BleClient
    private lateinit var body: LinearLayout
    private lateinit var statusView: TextView
    private lateinit var connectButton: Button
    private lateinit var nav: LinearLayout
    private lateinit var scroll: ScrollView
    private var pairStep = 0
    private var pairMessage: TextView? = null
    private var pairProgress: ProgressBar? = null
    private var pairRetry: Button? = null
    private var scanButton: Button? = null
    private var scanStarted = false
    private var pendingAddress = ""
    private var pendingName = ""
    private var whitePanel: LinearLayout? = null
    private var colorPanel: LinearLayout? = null
    private var whiteButton: Button? = null
    private var colorButton: Button? = null
    private var powerButton: Button? = null
    private var colorPreview: View? = null
    private var lightSurface: LightArtwork? = null
    private var favoriteList: LinearLayout? = null
    private var colorMode = false
    private var loadedSelection = ""
    private var deviceList: LinearLayout? = null
    private var pendingPermission: (() -> Unit)? = null
    private var active = false
    private var alive = true
    private var tab = "lights"
    private var selected = "all"
    private var reading = false
    private var refreshQueued = false
    private var editingUntil = 0L
    private var dragging = false
    private var overviewToken = 0
    private var detailIndex = 0
    private var demo: DemoLamp? = null
    private val lights = linkedMapOf<String, JSONObject>()
    private val groups = linkedMapOf<String, JSONObject>()
    private val scenes = linkedMapOf<String, JSONObject>()
    private var network = JSONObject()
    private var currentValues = defaults()
    private var wheel: ColorWheel? = null
    private var controls: LinearLayout? = null
    private val sliders = linkedMapOf<String, Pair<SeekBar, TextView>>()
    private var lightCards: LinearLayout? = null
    private var stateLabel: TextView? = null
    private var targets: Spinner? = null
    private var selecting = false
    private var displayedTargets = emptyList<Pair<String, String>>()
    private var sceneList: LinearLayout? = null
    private val devices = linkedMapOf<String, String>()
    private val modes = listOf("Duha plynule", "Barevné kroky", "Barevný dech", "Barevný flash", "Disko", "Párty rytmus", "Modrá a růžová", "Západ slunce", "Polární záře", "Oceán", "Oheň", "Barevný běžec", "Vlastní paleta")
    private val connected get() = demo != null || client.ready
    private fun dp(n: Int) = (n*resources.displayMetrics.density).roundToInt()
    private fun defaults() = JSONObject().put("on", false).put("level", 127).put("kelvin", 2700).put("hue", 0).put("saturation", 0)
    private fun json(vararg pairs: Pair<String, Any?>) = JSONObject().also { o -> pairs.forEach { o.put(it.first, it.second) } }
    private fun rounded(color: Int, radius: Int = 24) = GradientDrawable().apply { setColor(color); cornerRadius = dp(radius).toFloat() }
    private fun column() = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
    private fun label(text: String, size: Float = 16f, color: Int = ink) = TextView(this).apply { this.text = text; textSize = size; setTextColor(color); setPadding(0, dp(6), 0, dp(6)) }
    private fun icon(kind: String, color: Int = mint, size: Int = 22) = LampIcon(kind, color).apply { setBounds(0, 0, dp(size), dp(size)) }
    private fun styleButton(view: Button, primary: Boolean = false, quiet: Boolean = false) {
        view.setTextColor(if (primary) backgroundColor else ink)
        view.backgroundTintList = null
        val surface = if (primary) GradientDrawable(GradientDrawable.Orientation.TL_BR, intArrayOf(mint, Color.rgb(172, 223, 120))).apply { cornerRadius = dp(18).toFloat() }
            else rounded(if (quiet) Color.TRANSPARENT else Color.rgb(27, 35, 37), 18).apply { if (!quiet) setStroke(dp(1), Color.rgb(49, 62, 61)) }
        view.background = RippleDrawable(ColorStateList.valueOf(0x447FBD94), surface, null)
        view.isSelected = primary
    }
    private fun button(text: String, symbol: String? = null, primary: Boolean = false, job: () -> Unit) = Button(this).apply {
        this.text = text; isAllCaps = false; minHeight = dp(52); minimumWidth = 0; minWidth = 0; textSize = 14f
        typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL); letterSpacing = .01f
        setPadding(dp(14), dp(10), dp(14), dp(10)); stateListAnimator = null
        layoutParams = LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(4); bottomMargin = dp(4) }
        styleButton(this, primary)
        if (symbol != null) { setCompoundDrawablesRelative(icon(symbol, if (primary) backgroundColor else mint), null, null, null); compoundDrawablePadding = dp(8) }
        setOnClickListener { performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY); job() }
    }
    private fun row(vararg views: View) = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        views.forEachIndexed { i, v -> addView(v, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply { if (i < views.lastIndex) marginEnd = dp(8); topMargin = dp(4); bottomMargin = dp(4) }) }
    }
    private fun card(): LinearLayout = column().apply {
        background = rounded(panelColor, 28).apply { setStroke(dp(1), Color.rgb(40, 50, 51)) }; setPadding(dp(18), dp(16), dp(18), dp(18))
        body.addView(this, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = dp(14) })
    }
    private fun heading(parent: LinearLayout, text: String) { parent.addView(label(text, 26f).apply { typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL); letterSpacing = -.035f }) }
    private fun eyebrow(parent: LinearLayout, text: String) { parent.addView(label(text, 10f, mint).apply { letterSpacing = .16f; typeface = Typeface.create("monospace", Typeface.NORMAL) }) }
    private fun artwork(tone: Int = mint, second: Int = Color.rgb(101,226,224), orb: Boolean = false) = LightArtwork(resources.displayMetrics.density, orb).apply { this.tone = tone; this.second = second }
    private fun toast(text: String) { if (alive) Toast.makeText(this, text, Toast.LENGTH_LONG).show() }
    private fun error(e: Throwable) { toast(e.message ?: "Příkaz se nepodařil.") }
    override fun onCreate(saved: Bundle?) {
        super.onCreate(saved)
        client = app.client
        selected = saved?.getString("selected") ?: "all"
        pairStep = saved?.getInt("pairStep") ?: 0
        pendingAddress = saved?.getString("pendingAddress").orEmpty(); pendingName = saved?.getString("pendingName").orEmpty()
        colorMode = saved?.getBoolean("colorMode") ?: false
        val root = column().apply { setBackgroundColor(backgroundColor) }
        root.setOnApplyWindowInsetsListener { v, insets ->
            if (Build.VERSION.SDK_INT >= 30) {
                val i = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
                v.setPadding(i.left, i.top, i.right, i.bottom)
            } else v.setPadding(insets.systemWindowInsetLeft, insets.systemWindowInsetTop, insets.systemWindowInsetRight, insets.systemWindowInsetBottom)
            insets
        }
        val header = column().apply { setPadding(dp(22), dp(8), dp(22), dp(8)) }
        connectButton = button("Připojit", "bluetooth") { showConnection() }
        header.addView(row(label("Z-lamp", 31f).apply {
            typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL); letterSpacing = -.06f
            text = SpannableString("Z-lamp").apply { setSpan(ForegroundColorSpan(mint),0,1,Spanned.SPAN_EXCLUSIVE_EXCLUSIVE) }
        }, connectButton))
        statusView = label(client.status, 13f, muted).apply { accessibilityLiveRegion = View.ACCESSIBILITY_LIVE_REGION_POLITE }
        header.addView(statusView); root.addView(header)
        body = column().apply { setPadding(dp(16), dp(6), dp(16), dp(12)) }
        scroll = ScrollView(this).apply { isFillViewport = true; isVerticalScrollBarEnabled = false; addView(body) }
        root.addView(scroll, LinearLayout.LayoutParams(-1, 0, 1f))
        nav = row(button("Světla") { show("lights") }, button("Scény") { show("scenes") }, button("Nastavení") { show("settings") })
        nav.setPadding(dp(6), dp(2), dp(6), dp(2)); nav.background = rounded(Color.rgb(21, 29, 30), 26).apply { setStroke(dp(1), Color.rgb(43, 57, 56)) }
        root.addView(FrameLayout(this).apply {
            setPadding(dp(12), dp(6), dp(12), dp(8)); addView(nav, FrameLayout.LayoutParams(-1, -2))
        }); setContentView(root)
        client.onStatus = { if (alive) {
            statusView.text = when { demo != null -> "UKÁZKA · bez připojení k lampě"; client.ready -> "${client.savedName} · Bluetooth připojeno"; client.scanning -> "Hledám lampy v okolí…"; client.connecting -> "Zabezpečené připojení…"; else -> "Nepřipojeno" }
            if (!connected && !client.connecting) { overviewToken++; reading = false }
            updateEnabled(); updatePairing()
        } }
        client.onReady = { if (alive) { lights.clear(); groups.clear(); scenes.clear(); network = JSONObject(); loadedSelection = ""; show("lights"); refresh() } }
        client.onChanged = { if (alive && active) scheduleRefresh() }
        if (saved?.getBoolean("demo") == true) startDemo()
        else if (client.ready) { show(saved?.getString("tab") ?: "lights"); refresh() }
        else showConnection(if (client.connecting) 2 else pairStep.coerceAtMost(1))
        if (intent.getBooleanExtra("arrival", false)) arrivalSettings()
        if (intent.getBooleanExtra("tile", false) || intent.action == android.service.quicksettings.TileService.ACTION_QS_TILE_PREFERENCES) tileSettings()
    }
    override fun onStart() { super.onStart(); active = true; app.uiVisible = true
        app.calibrating = arrivalDialog?.isShowing == true
        if (app.arrivalPrefs.getBoolean("enabled", false) && !app.arrivalRunning && ArrivalService.permitted(this)) {
            try { ArrivalService.start(this) } catch (e: RuntimeException) { app.arrivalStatus = "Příchod se nespustil: ${e.message}" }
        }
         if (::client.isInitialized) {
        if (connected) scheduleRefresh()
        else if (!client.connecting && !client.scanning) statusView.text = "Nepřipojeno"
        updatePairing()
    } }
    override fun onStop() { active = false; app.uiVisible = false; app.choosingLamp = false; app.calibrating = false; app.arrival.resetSignal(); client.stopScan(); updatePairing(); super.onStop() }
    override fun onSaveInstanceState(out: Bundle) { out.putString("selected", selected); out.putString("tab", tab); out.putInt("pairStep", pairStep); out.putString("pendingAddress", pendingAddress); out.putString("pendingName", pendingName); out.putBoolean("colorMode", colorMode); out.putBoolean("demo", demo != null); super.onSaveInstanceState(out) }
    override fun onBackPressed() {
        when { tab == "connect" && client.autoConnecting -> super.onBackPressed(); tab == "connect" && client.connecting -> { ArrivalService.stop(this); client.disconnect(); showConnection(1) }; tab == "connect" && pairStep > 0 -> showConnection(pairStep-1); tab == "connect" && connected -> show("lights"); tab != "lights" && tab != "connect" -> show("lights"); else -> super.onBackPressed() }
    }
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent); setIntent(intent)
        if (intent.getBooleanExtra("arrival", false)) arrivalSettings()
        if (intent.getBooleanExtra("tile", false) || intent.action == android.service.quicksettings.TileService.ACTION_QS_TILE_PREFERENCES) tileSettings()
    }
    override fun onDestroy() {
        alive = false; handler.removeCallbacksAndMessages(null); client.onReady = null; client.onChanged = null; client.onStatus = null
        arrivalDialog?.dismiss()
        if (!isChangingConfigurations && !app.companionPresent && !app.arrivalPrefs.getBoolean("enabled", false) && !app.tileBusy && !app.alarmBusy && !app.tileListening) client.disconnect()
        super.onDestroy()
    }
    private fun withBluetooth(scan: Boolean = false, action: () -> Unit) {
        if (!client.available) { toast("Telefon nemá Bluetooth LE."); return }
        val needed = if (Build.VERSION.SDK_INT >= 31) arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
            else if (scan) arrayOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION) else emptyArray()
        val missing = needed.filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (missing.isNotEmpty()) { pendingPermission = { withBluetooth(scan, action) }; requestPermissions(missing.toTypedArray(), 7); return }
        if (!client.bluetoothEnabled) { pendingPermission = { withBluetooth(scan, action) }; startActivityForResult(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE), 8); return }
        if (scan && Build.VERSION.SDK_INT < 31) {
            val manager = getSystemService(LocationManager::class.java)
            val enabled = if (Build.VERSION.SDK_INT >= 28) manager.isLocationEnabled else manager.isProviderEnabled(LocationManager.GPS_PROVIDER) || manager.isProviderEnabled(LocationManager.NETWORK_PROVIDER)
            if (!enabled) { toast("Android 8–11 vyžaduje pro hledání BLE zapnutou polohu. Aplikace ji neukládá."); startActivity(Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS)); return }
        }
        try { action() } catch (_: SecurityException) { toast("Povol aplikaci přístup k Bluetooth v nastavení telefonu.") }
    }
    override fun onRequestPermissionsResult(code: Int, permissions: Array<out String>, grants: IntArray) {
        super.onRequestPermissionsResult(code, permissions, grants)
        if (code == 7) { val next = pendingPermission; pendingPermission = null; if (grants.isNotEmpty() && grants.all { it == PackageManager.PERMISSION_GRANTED }) next?.invoke() else toast("Bez oprávnění nelze připojit lampu. Povolení můžeš změnit v nastavení aplikace.") }
    }
    override fun onActivityResult(request: Int, result: Int, data: Intent?) {
        super.onActivityResult(request, result, data)
        if (request == 8) { val next = pendingPermission; pendingPermission = null; if (result == RESULT_OK) next?.invoke() }
        if (request == 9) {
            val settings = getSharedPreferences("companion", MODE_PRIVATE)
            val address = settings.getString("pending", "").orEmpty()
            settings.edit().remove("pending").apply()
            if (result == RESULT_OK && address.isNotBlank() && Build.VERSION.SDK_INT >= 31) {
                try { CompanionLink.enable(this, address); toast("Android nyní hlídá návrat lampy do dosahu.") }
                catch (e: Exception) { error(e) }
            }
        }
    }
    private fun showConnection(step: Int = 0) {
        app.choosingLamp = step == 1
        pairStep = step; tab = "connect"; resetView(); nav.visibility = View.GONE
        if (step != 1) client.stopScan()
        val hero = card()
        hero.background = artwork()
        val insignia = label("SVĚTLO. PODLE TEBE.", 10f, mint).apply {
            typeface = Typeface.MONOSPACE; letterSpacing = .12f; gravity = Gravity.CENTER_VERTICAL
            setCompoundDrawablesRelative(null, null, icon("brand", mint, 44), null); compoundDrawablePadding = dp(10)
        }; hero.addView(insignia)
        heading(hero, when (step) { 1 -> "Najdeme tvoji lampu."; 2 -> "Už jen propojit."; else -> "Světlo na dosah." })
        hero.addView(label(when (step) { 1 -> "Vyber lampu, kterou chceš ovládat. Hledáme zařízení Z-lamp."; 2 -> "Připojení bez PINu. Za chvíli můžeš ovládat světla."; else -> "Barvy, které sednou tvé náladě. Ovládání přes Bluetooth, bez účtu a cloudu." }, 16f, muted))
        val steps = row(*listOf("Připravit", "Najít", "Propojit").mapIndexed { i, title -> label(title, 12f, if (i == step) ink else muted).apply {
            gravity = Gravity.CENTER; setPadding(dp(4), dp(12), dp(4), dp(12)); isSelected = i == step
            background = rounded(if (i == step) Color.rgb(48, 64, 41) else Color.TRANSPARENT, 12).apply { if (i == step) setStroke(dp(1), Color.rgb(112, 145, 71)) }
        } }.toTypedArray()); hero.addView(steps)
        if (connected) hero.addView(button("Zpět k ovládání", "chevron") { show("lights") })
        val c = card()
        when (step) {
            0 -> {
                if (client.savedAddress.isNotBlank()) {
                    eyebrow(c, "POSLEDNÍ LAMPA")
                    c.addView(button("Připojit ${client.savedName}", "bluetooth", true) { connectTo(client.savedAddress, client.savedName) })
                    c.addView(label("Klepnutím se znovu připojíš.", 13f, muted))
                }
                heading(c, "Připoj svoji lampu.")
                c.addView(label("Stiskni Párovat a klepni na lampu v seznamu. Připojení proběhne bez PINu; případné potvrzení Androidu přijmi.", 15f, muted))
                c.addView(button("Párovat", "bluetooth", true) { showConnection(1); scanLamps() })
            }
            1 -> {
                heading(c, "Lampy v okolí")
                pairProgress = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply { isIndeterminate = true }; c.addView(pairProgress, LinearLayout.LayoutParams(-1, dp(4)))
                pairMessage = label("Připraveno hledat. Android si může vyžádat oprávnění Bluetooth.", 14f, muted); c.addView(pairMessage)
                deviceList = column(); c.addView(deviceList); renderDevices()
                scanButton = button("Hledat lampy", "bluetooth", true) { scanLamps() }; c.addView(scanButton)
                c.addView(button("Zpět") { showConnection(0) })
            }
            else -> {
                heading(c, pendingName.ifBlank { "Připojení k lampě" })
                pairProgress = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply { isIndeterminate = true }; c.addView(pairProgress, LinearLayout.LayoutParams(-1, dp(4)))
                pairMessage = label(client.status, 15f); c.addView(pairMessage)
                c.addView(label("Vše vyřídíš tady v aplikaci. Pokud Android zobrazí potvrzení párování, přijmi ho.", 14f, muted))
                pairRetry = button("Zkusit připojit znovu", "bluetooth", true) { if (pendingAddress.isNotBlank()) connectTo(pendingAddress, pendingName) else showConnection(1) }; c.addView(pairRetry)
                c.addView(button("Zrušit a zpět na hledání") { client.disconnect(); showConnection(1) })
            }
        }
        val help = card()
        if (step > 0) help.addView(button("Nedaří se připojit?") { pairingHelp() })
        if (client.savedAddress.isNotBlank()) help.addView(button("Systémové připojování", "bluetooth") { companionSettings() })
        help.addView(button("Automatický příchod", "bluetooth") { arrivalSettings() })
        help.addView(button("Prohlédnout ukázku bez lampy", "bulb") { startDemo() })
        updatePairing()
    }
    private fun connectTo(address: String, name: String) {
        withBluetooth {
            if (address != client.savedAddress) { ArrivalService.stop(this); CompanionLink.disable(this, forget = true) }
            demo = null; pendingAddress = address; pendingName = name
            if (client.ready && client.connectionAddress == address) { show("lights"); refresh() }
            else { showConnection(2); client.connect(address); updatePairing() }
        }
    }
    private fun scanLamps() {
        withBluetooth(scan = true) {
            app.choosingLamp = true; demo = null; client.disconnect(); scanStarted = true; devices.clear(); renderDevices()
            client.scan { device, name, rssi ->
                devices[device.address] = "$name\n${if (rssi >= -65) "Silný signál" else if (rssi >= -82) "Dobrý signál" else "Slabý signál"}"
                renderDevices(); updatePairing()

            }
        }
    }
    private fun renderDevices() {
        deviceList?.let { list -> list.removeAllViews(); devices.forEach { (address, title) ->
            list.addView(button(title, "bulb") { connectTo(address, title.substringBefore('\n')) }.apply { gravity = Gravity.START or Gravity.CENTER_VERTICAL })
        } }
    }
    private fun updatePairing() {
        if (tab != "connect") return
        pairProgress?.visibility = if (client.scanning || client.connecting) View.VISIBLE else View.GONE
        scanButton?.isEnabled = !client.scanning
        scanButton?.text = if (client.scanning) "Hledám…" else if (scanStarted) "Hledat znovu" else "Hledat lampy"
        pairRetry?.visibility = if (client.connecting || client.ready) View.GONE else View.VISIBLE
        pairMessage?.text = if (pairStep == 2) client.status else when {
            client.scanning -> if (devices.isEmpty()) "Hledám… obvykle to trvá pár sekund." else "Vyber svou lampu. Hledání ještě pokračuje."
            !scanStarted -> "Stiskni Hledat lampy. Android si může vyžádat oprávnění Bluetooth."
            client.status.startsWith("Hledání selhalo") -> client.status
            devices.isEmpty() -> "Žádná lampa zatím nenalezena. Ověř napájení a zapnuté Bluetooth v lampě, přibliž telefon a hledej znovu."
            else -> "Nalezeno: ${devices.size}. Klepni na svou lampu."
        }
        pairMessage?.accessibilityLiveRegion = View.ACCESSIBILITY_LIVE_REGION_POLITE
    }
    internal fun pairingUri(value: String): Uri {
        val raw = value.trim()
        val uri = Uri.parse(if (raw.contains("://")) raw else "http://$raw")
        val host = uri.host.orEmpty()
        require(uri.scheme in listOf("http", "https") && host.isNotBlank() && uri.userInfo == null &&
            Regex("[a-zA-Z0-9.-]+").matches(host) && (uri.port == -1 || uri.port in 1..65535)) { "Zadej IP adresu nebo název lampy, například 192.168.4.1." }
        if (host.all { it.isDigit() || it == '.' }) require(host.split('.').size == 4 && host.split('.').all { it.toIntOrNull()?.let { n -> n in 0..255 } == true }) { "IP adresa musí mít čtyři čísla od 0 do 255." }
        return uri.buildUpon().path("/").clearQuery().fragment("bluetooth").build()
    }
    private fun openPairingWeb() {
        val c = column()
        c.addView(label("Zadej IP z displeje lampy. Telefon musí být ve stejné Wi-Fi nebo v hotspotu lampy. Otevře se nastavení lampy; k párování telefonu web nepotřebuješ.", 15f, muted))
        val address = edit(c, "Adresa lampy", prefs.getString("webAddress", "").orEmpty()).apply { hint = "např. 192.168.4.1"; inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI }
        lateinit var dialog: AlertDialog
        c.addView(button("Otevřít web", "web", true) {
            try {
                val uri = pairingUri(address.text.toString()); startActivity(Intent(Intent.ACTION_VIEW, uri))
                prefs.edit().putString("webAddress", uri.buildUpon().fragment(null).build().toString()).apply(); dialog.dismiss()
            } catch (_: ActivityNotFoundException) { toast("V telefonu není dostupný webový prohlížeč.") }
            catch (e: IllegalArgumentException) { address.error = e.message }
        })
        dialog = form("Web lampy", c)
    }
    private fun pairingHelp() {
        val c = column()
        c.addView(label("1. Zapni lampu a Bluetooth telefonu.\n\n2. V aplikaci stiskni Párovat a klepni na svoji lampu.\n\n3. Pokud Android zobrazí potvrzení, přijmi ho. PIN ani otevření webu nejsou potřeba.\n\nTento postup používá firmware Wi-Fi 2.9.2 nebo novější. Lampa obsluhuje jeden telefon současně; případný druhý odpoj. Pokud bylo párování smazané jen v lampě a telefon odmítá obnovu, odeber starý záznam v Bluetooth telefonu a vyber lampu znovu.", 15f, muted))
        c.addView(button("Oprávnění aplikace", "settings") { startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:$packageName"))) })
        c.addView(button("Bluetooth v telefonu", "bluetooth") { startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS)) })
        form("Pomoc s připojením", c)
    }
    internal fun startDemo() {
        ArrivalService.stop(this); CompanionLink.disable(this); client.disconnect(); demo = DemoLamp(); lights.clear(); groups.clear(); scenes.clear(); loadedSelection = ""
        statusView.text = "UKÁZKA · bez připojení k lampě"; show("lights"); refresh()
    }
    private fun resetView() {
        body.removeAllViews(); scroll.scrollTo(0, 0); controls = null; wheel = null; lightCards = null; targets = null; stateLabel = null; sceneList = null; deviceList = null; sliders.clear()
        pairMessage = null; pairProgress = null; pairRetry = null; scanButton = null; whitePanel = null; colorPanel = null; whiteButton = null; colorButton = null; powerButton = null; colorPreview = null; favoriteList = null; lightSurface = null
        dragging = false
    }
    private fun show(which: String) {
        if (!connected && lights.isEmpty() && which != "settings") { showConnection(); return }
        tab = which; resetView(); nav.visibility = View.VISIBLE
        listOf("lights" to "bulb", "scenes" to "scenes", "settings" to "settings").forEachIndexed { index, item ->
            (nav.getChildAt(index) as Button).apply {
                styleButton(this, quiet = true); isSelected = which == item.first
                setTextColor(if (isSelected) mint else muted); textSize = 12f
                setPadding(dp(4), dp(10), dp(4), dp(10))
                background = RippleDrawable(ColorStateList.valueOf(0x447FBD94), rounded(if (isSelected) Color.rgb(47, 63, 38) else Color.TRANSPARENT, 22).apply { if (isSelected) setStroke(dp(1), Color.rgb(95, 116, 60)) }, null)
                setCompoundDrawables(null, icon(item.second, if (isSelected) mint else muted), null, null); compoundDrawablePadding = dp(4)
            }
        }
        when (which) { "lights" -> showLights(); "scenes" -> showScenes(); else -> showSettings() }
        updateEnabled()
    }
    private fun updateEnabled() {
        connectButton.text = if (connected) "Spojení" else "Připojit"
        fun enabled(view: View) { view.isEnabled = connected; if (view is ViewGroup) for (i in 0 until view.childCount) enabled(view.getChildAt(i)) }
        controls?.let { enabled(it); it.alpha = if (connected) 1f else .45f }
        if (!connected && tab != "connect") stateLabel?.text = "Odpojeno · zobrazené hodnoty nejsou aktuální."
    }
    private fun call(op: String, args: JSONObject = JSONObject(), done: (JSONObject) -> Unit = {}) {
        if (!connected) { toast("Nejdřív připoj lampu."); return }
        val preview = demo
        if (preview != null) { try { done(preview.handle(op, args)) } catch (e: Exception) { error(e) }; return }
        client.request(op, args) { result -> if (alive) result.onSuccess(done).onFailure(::error) }
    }
    private fun mutate(op: String, args: JSONObject, done: () -> Unit = {}) { call(op, args) { toast(if (demo != null) "Změna pouze v ukázce." else if (op == "remove" || op == "startup") "Požadavek přijat. Výsledek ověř ve stavu žárovky." else "Uloženo v lampě."); done(); refresh() } }
    private fun scheduleRefresh() {
        if (refreshQueued) return
        refreshQueued = true
        handler.postDelayed({ refreshQueued = false; if (active && connected) refresh() }, 1500)
    }
    private fun page(op: String, offset: Int = 0, collected: MutableList<JSONObject> = mutableListOf(), done: (List<JSONObject>) -> Unit) {
        val token = overviewToken
        val callback: (Result<JSONObject>) -> Unit = callback@{ result ->
            if (!alive || token != overviewToken) return@callback
            result.onFailure { reading = false; error(it) }.onSuccess { data ->
                try {
                    val items = data.getJSONArray("items"); require(items.length() <= 4 && collected.size+items.length() <= 17)
                    for (i in 0 until items.length()) collected.add(items.getJSONObject(i))
                    if (data.isNull("next")) done(collected)
                    else { val next = data.getInt("next"); require(next > offset && next <= 17); page(op, next, collected, done) }
                } catch (e: Exception) { reading = false; error(e) }
            }
        }
        if (demo != null) callback(runCatching { demo!!.handle(op, json("offset" to offset)) })
        else client.request(op, json("offset" to offset), callback)
    }
    private fun refresh() {
        if (!connected || reading) return
        reading = true; overviewToken++
        page("list_lights") { items ->
            val ids = items.map { it.getString("id") }.toSet(); lights.keys.retainAll(ids)
            items.forEach { summary -> val id = summary.getString("id"); val old = lights[id] ?: JSONObject(); summary.keys().forEach { old.put(it, summary.get(it)) }; lights[id] = old }
            page("list_groups") { gs ->
                groups.clear(); gs.forEach { groups[it.getString("id")] = it }
                page("list_scenes") { ss ->
                    scenes.clear(); ss.forEach { scenes[it.getString("id")] = it }; reading = false
                    updateTargets(); renderCards(); renderScenes(); readSelected()
                    if (tab == "scenes") readSceneDetails()
                    if (tab == "settings") readNetwork()
                    val other = lights.keys.toList().getOrNull((detailIndex++ % maxOf(1, lights.size)))
                    if (other != null && other != selected) readLight(other) { renderCards() }
                }
            }
        }
    }
    private fun readLight(id: String, done: (JSONObject) -> Unit) { call("get_light", json("id" to id)) { light -> lights[id] = light; done(light) } }
    private fun readSelected() {
        val selection = selected
        fun apply(light: JSONObject) {
            if (selection != selected) return
            if (!dragging) sliders["kelvin"]?.first?.let { slider ->
                val lower = if (selected in lights) light.optInt("minKelvin", 2000).coerceIn(2000, 6500) else 2000
                val upper = if (selected in lights) light.optInt("maxKelvin", 6500).coerceIn(lower, 6500) else 6500
                slider.max = 6500; slider.min = lower; slider.max = upper
            }
            if (!dragging && SystemClock.elapsedRealtime() > editingUntil) {
                val wanted = JSONObject((light.optJSONObject("wanted") ?: defaults()).toString())
                if (loadedSelection != selected || wanted.optInt("saturation") != currentValues.optInt("saturation")) colorMode = wanted.optInt("saturation") > 0
                currentValues = wanted; loadedSelection = selected; renderControls()
            }
            val on = light.optBoolean("on"); val actual = if (!light.optBoolean("reported")) "Čekám na stav" else if (on) "Svítí · ${percent(light.optInt("level"))} %" else "Zhasnuto"
            stateLabel?.text = if (selected == "all" || selected in groups) "Celý výběr · hodnoty podle ${light.optString("name")}."
                else if (!light.optBoolean("online")) "Nedostupná · poslední požadované hodnoty" else "$actual · nastavení níže je požadované"
            renderCards()
        }
        when {
            selected in lights -> readLight(selected, ::apply)
            selected in groups -> call("get_group", json("id" to selected)) { g -> groups[g.getString("id")] = g; val id = g.optJSONArray("members")?.optString(0).orEmpty(); if (id.isNotEmpty()) readLight(id, ::apply) }
            else -> lights.keys.firstOrNull { it != "display" }?.let { readLight(it, ::apply) }
        }
    }
    private fun targetItems(): List<Pair<String, String>> = listOf("all" to "Všechny žárovky") + groups.map { it.key to "Skupina · ${it.value.optString("name")}" } + lights.map { it.key to it.value.optString("name") }
    private fun spinner(names: List<String>) = Spinner(this).apply {
        adapter = ArrayAdapter(this@MainActivity, android.R.layout.simple_spinner_dropdown_item, names); minimumHeight = dp(52)
        backgroundTintList = ColorStateList.valueOf(mint)
    }
    private fun updateTargets() {
        val items = targetItems(); if (items.none { it.first == selected }) selected = "all"
        targets?.let { s ->
            selecting = true
            if (displayedTargets != items) { displayedTargets = items; s.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, items.map { it.second }) }
            s.setSelection(items.indexOfFirst { it.first == selected }); selecting = false
        }
    }
    private fun select(id: String) { selected = id; editingUntil = 0; updateTargets(); renderFavorites(); readSelected() }
    private fun percent(level: Int) = (level*100f/254).roundToInt().coerceIn(1, 100)
    private fun showLights() {
        val c = card(); controls = c
        lightSurface = artwork(); c.background = lightSurface
        eyebrow(c, "NASTAV SI ATMOSFÉRU")
        displayedTargets = emptyList(); targets = spinner(targetItems().map { it.second }); c.addView(targets); updateTargets()
        targets!!.contentDescription = "Vybrat světlo nebo skupinu"
        targets!!.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onNothingSelected(parent: AdapterView<*>?) {}
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                val target = displayedTargets.getOrNull(position)?.first ?: return
                if (!selecting && target != selected) select(target)
            }
        }
        val powerRow = LinearLayout(this).apply { gravity = Gravity.CENTER_VERTICAL; setPadding(0, dp(10), 0, dp(4)) }
        colorPreview = View(this).apply { background = artwork(orb = true); contentDescription = "Náhled požadované barvy"; importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO }
        powerRow.addView(colorPreview, LinearLayout.LayoutParams(dp(72), dp(72)).apply { marginEnd = dp(12) })
        powerButton = button("Rozsvítit", "power", true) { patch(json("on" to !currentValues.optBoolean("on"))) }
        powerRow.addView(powerButton, LinearLayout.LayoutParams(0, -2, 1f)); c.addView(powerRow)
        stateLabel = label("Vyber světlo nebo skupinu.", 13f, muted); c.addView(stateLabel)
        addSlider(c, "level", "Jas", 1, 100) { "$it %" }
        whiteButton = button("Bílá", "sun") { colorMode = false; patch(json("saturation" to 0)) }
        colorButton = button("Barva", "scenes") { colorMode = true; patch(json("saturation" to (currentValues.optInt("saturation").takeIf { it > 0 } ?: 100))) }
        c.addView(row(whiteButton!!, colorButton!!))
        whitePanel = column(); c.addView(whitePanel)
        addSlider(whitePanel!!, "kelvin", "Teplota bílé", 2000, 6500) { "$it K" }
        whitePanel!!.addView(row(button("Teplá") { patch(json("kelvin" to 2700, "saturation" to 0)); colorMode = false; renderControls() }, button("Denní") { patch(json("kelvin" to 4000, "saturation" to 0)); colorMode = false; renderControls() }, button("Studená") { patch(json("kelvin" to 6500, "saturation" to 0)); colorMode = false; renderControls() }))
        colorPanel = column(); c.addView(colorPanel)
        wheel = ColorWheel(this).apply {
            onColor = { h, s, done -> dragging = !done; patch(json("hue" to h, "saturation" to s)) }
        }; colorPanel!!.addView(wheel)
        addSlider(colorPanel!!, "saturation", "Sytost", 0, 100) { "$it %" }
        colorPanel!!.addView(button("Plná barva") { patch(json("saturation" to 100)) })
        val exact = column().apply { visibility = View.GONE }
        colorPanel!!.addView(row(button("Přesný odstín") { exact.visibility = if (exact.visibility == View.VISIBLE) View.GONE else View.VISIBLE }, button("Bílá pod barvou") { whiteMixEditor() }))
        addSlider(exact, "hue", "Odstín", 0, 359) { "$it°" }; colorPanel!!.addView(exact)
        favoriteList = column(); c.addView(favoriteList); renderFavorites()
        c.addView(button("Nastavení vybraného světla", "settings") { if (selected in lights) lightSettings(selected) else toast("Nejdřív vyber konkrétní světlo.") })
        c.addView(button("Časovač vypnutí", "settings") { timerSettings() })
        val cards = card(); eyebrow(cards, "CELÁ SESTAVA"); heading(cards, "Tvoje světla"); lightCards = column(); cards.addView(lightCards)
        cards.addView(row(button("Obnovit") { refresh() }, button("Skupiny") { groupList() }))
        renderControls(); renderCards(); if (connected) readSelected()
    }
    private fun addSlider(parent: LinearLayout, key: String, title: String, min: Int, max: Int, format: (Int) -> String) {
        val caption = label(title, if (key == "level") 38f else 16f).apply { typeface = Typeface.create("sans-serif-light", Typeface.NORMAL) }
        if (key == "level") parent.addView(row(label(title.uppercase(), 11f, muted).apply { typeface = Typeface.MONOSPACE; letterSpacing = .16f }, caption.apply { gravity = Gravity.END; letterSpacing = -.06f }))
        else parent.addView(caption)
        val captionText: (Int) -> String = { if (key == "level") format(it) else "$title   ${format(it)}" }
        val seek = LightSlider(this, key).apply { this.min = min; this.max = max; contentDescription = title; keyProgressIncrement = if (key == "kelvin") 50 else 1 }
        parent.addView(seek); sliders[key] = seek to caption
        seek.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onStartTrackingTouch(s: SeekBar) { dragging = true }
            override fun onStopTrackingTouch(s: SeekBar) { dragging = false; editingUntil = SystemClock.elapsedRealtime()+1500; scheduleRefresh() }
            override fun onProgressChanged(s: SeekBar, value: Int, user: Boolean) {
                caption.text = captionText(value)
                if (user) patch(json(key to if (key == "level") (value*254f/100).roundToInt().coerceIn(1, 254) else value))
            }
        })
        caption.tag = captionText
    }
    private fun renderControls() {
        sliders.forEach { (key, pair) ->
            val value = if (key == "level") percent(currentValues.optInt(key, 127)) else currentValues.optInt(key)
            pair.first.progress = value
            @Suppress("UNCHECKED_CAST") val caption = pair.second.tag as (Int) -> String
            pair.second.text = caption(pair.first.progress)
        }
        wheel?.apply { hue = currentValues.optInt("hue").toFloat(); saturation = currentValues.optInt("saturation").toFloat(); kelvin = currentValues.optInt("kelvin", 2700); invalidate() }
        val pure = Color.HSVToColor(floatArrayOf(currentValues.optInt("hue").toFloat(), 1f, 1f))
        (sliders["saturation"]?.first as? LightSlider)?.tint = pure
        (sliders["level"]?.first as? LightSlider)?.tint = if (currentValues.optInt("saturation") == 0) Color.rgb(246, 201, 132) else pure
        val preview = previewColor(currentValues)
        (colorPreview?.background as? LightArtwork)?.tone = preview
        lightSurface?.tone = preview
        lightSurface?.second = LightArtwork.mix(preview, Color.rgb(101, 226, 224), .4f)
        powerButton?.apply {
            val on = currentValues.optBoolean("on"); text = if (on) "Zhasnout" else "Rozsvítit"; styleButton(this, on)
            setCompoundDrawablesRelative(icon("power", if (on) backgroundColor else mint), null, null, null)
        }
        whiteButton?.let { styleButton(it, !colorMode); it.setCompoundDrawablesRelative(icon("sun", if (!colorMode) backgroundColor else mint), null, null, null) }
        colorButton?.let { styleButton(it, colorMode); it.setCompoundDrawablesRelative(icon("scenes", if (colorMode) backgroundColor else mint), null, null, null) }
        whitePanel?.visibility = if (!colorMode) View.VISIBLE else View.GONE
        colorPanel?.visibility = if (colorMode) View.VISIBLE else View.GONE
    }
    private fun whiteMixEditor() {
        val c = column()
        c.addView(label("Při nižší sytosti se barva míchá s touto bílou. Při 100% sytosti teplota výslednou barvu neovlivňuje.", 15f, muted))
        val caption = label("${currentValues.optInt("kelvin", 2700)} K", 22f); c.addView(caption)
        val slider = LightSlider(this, "kelvin").apply {
            max = 6500; min = sliders["kelvin"]?.first?.min ?: 2000; max = sliders["kelvin"]?.first?.max ?: 6500
            progress = currentValues.optInt("kelvin", 2700); contentDescription = "Teplota přimíchané bílé"; keyProgressIncrement = 50
        }
        slider.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onStartTrackingTouch(s: SeekBar) { dragging = true }
            override fun onStopTrackingTouch(s: SeekBar) { dragging = false; scheduleRefresh() }
            override fun onProgressChanged(s: SeekBar, value: Int, user: Boolean) { caption.text = "$value K"; if (user) patch(json("kelvin" to value)) }
        })
        c.addView(slider); form("Bílá pod barvou", c).setOnDismissListener { dragging = false }
    }
    private fun previewColor(values: JSONObject): Int {
        val color = Color.HSVToColor(floatArrayOf(values.optInt("hue").toFloat(), 1f, 1f))
        val white = ColorWheel.white(values.optInt("kelvin", 2700)); val s = values.optInt("saturation") / 100f
        fun mix(a: Int, b: Int) = (a*s + b*(1-s)).roundToInt().coerceIn(0,255)
        return Color.rgb(mix(Color.red(color),Color.red(white)),mix(Color.green(color),Color.green(white)),mix(Color.blue(color),Color.blue(white)))
    }
    private fun favoriteKey() = "favorites:${if (demo != null) "demo" else client.savedAddress}:$selected"
    private fun favorites(): JSONArray = runCatching { JSONArray(prefs.getString(favoriteKey(), "[]")) }.getOrDefault(JSONArray())
    private fun renderFavorites() {
        val list = favoriteList ?: return; list.removeAllViews(); list.addView(label("Rychlé barvy", 14f, muted))
        val strip = LinearLayout(this)
        fun swatch(title: String, values: JSONObject, remove: (() -> Unit)? = null) {
            val c = column().apply { gravity = Gravity.CENTER_HORIZONTAL }
            val color = previewColor(values)
            val b = button("", primary = false) { colorMode = values.optInt("saturation") > 0; patch(JSONObject(values.toString())) }.apply {
                contentDescription = "$title · ${if (values.optInt("saturation") == 0) "${values.optInt("kelvin")} kelvin" else "odstín ${values.optInt("hue")} stupňů"}"
                background = RippleDrawable(ColorStateList.valueOf(0x66FFFFFF), artwork(color, orb = true), rounded(Color.WHITE,24))
                if (remove != null) setOnLongClickListener { confirm("Odstranit oblíbenou barvu $title z tohoto telefonu?", remove); true }
            }
            c.addView(b, LinearLayout.LayoutParams(dp(48), dp(48)))
            c.addView(label(title, 11f, muted).apply { gravity = Gravity.CENTER; maxLines = 1 })
            strip.addView(c, LinearLayout.LayoutParams(dp(66), -2))
        }
        listOf("Čtení" to json("saturation" to 0, "kelvin" to 4000), "Večer" to json("saturation" to 0, "kelvin" to 2200), "Oceán" to json("hue" to 215, "saturation" to 100), "Růžová" to json("hue" to 320, "saturation" to 100)).forEach { swatch(it.first, it.second) }
        val saved = favorites()
        for (i in 0 until saved.length()) {
            val item = saved.optJSONObject(i) ?: continue; val values = item.optJSONObject("values") ?: continue
            swatch(item.optString("name", "Moje"), values) { val current = favorites(); current.remove(i); prefs.edit().putString(favoriteKey(), current.toString()).apply(); renderFavorites() }
        }
        list.addView(HorizontalScrollView(this).apply { isHorizontalScrollBarEnabled = false; addView(strip) })
        list.addView(button("Uložit oblíbenou", "heart") {
            val key = favoriteKey(); val savedColors = favorites(); val values = JSONObject(currentValues.toString()).apply { remove("on") }
            if (savedColors.length() >= 6) { toast("Uloženo je šest barev. Podrž některou vlastní barvu pro odstranění."); return@button }
            val c = column(); val title = edit(c, "Název oblíbené barvy", "Moje barva")
            c.addView(label("Uloží se barva, teplota a jas pro tento výběr světel. Oblíbené jsou pouze v tomto telefonu. Podržením vlastní barvy ji odebereš.", 14f, muted))
            lateinit var dialog: AlertDialog
            dialog = form("Oblíbená barva", c) { savedColors.put(json("name" to name(title), "values" to values)); prefs.edit().putString(key, savedColors.toString()).apply(); renderFavorites(); dialog.dismiss() }
        })
    }
    private fun patch(value: JSONObject) {
        if (!connected) return
        val target = selected // Freeze the target at the gesture; later selection never retargets queued writes.
        value.keys().forEach { currentValues.put(it, value.get(it)) }; renderControls(); editingUntil = SystemClock.elapsedRealtime()+1500
        if (demo != null) { call("control", json("target" to target, "patch" to value)) { readSelected() }; return }
        client.control(target, value) { result -> if (alive) { result.onFailure(::error); scheduleRefresh() } }
    }
    private fun renderCards() {
        val parent = lightCards ?: return; parent.removeAllViews()
        if (lights.isEmpty()) parent.addView(label("Zatím žádná světla. Žárovky přidáš v Nastavení.", 14f, muted))
        lights.forEach { (id, l) ->
            val status = if (!l.optBoolean("online")) "Nedostupná" else if (!l.has("wanted")) "Dostupná · vyber pro načtení" else if (l.optBoolean("on")) "Svítí · ${percent(l.optInt("level"))} %" else "Zhasnuto"
            val line = button("${l.optString("name")}\n$status", "bulb") { select(id) }.apply {
                gravity = Gravity.START or Gravity.CENTER_VERTICAL; isSelected = selected == id
                val color = if (l.optBoolean("on") && l.has("wanted")) previewColor(l.getJSONObject("wanted")) else muted
                setCompoundDrawablesRelative(icon("bulb", color, 28), null, icon("chevron", muted, 16), null)
                if (isSelected) background = RippleDrawable(ColorStateList.valueOf(0x447FBD94), rounded(Color.rgb(36, 50, 38), 18).apply { setStroke(dp(1), Color.rgb(115, 145, 73)) }, null)
            }
            parent.addView(line)
        }
    }
    private fun showScenes() {
        val c = card(); controls = c; eyebrow(c, "VYBER SI NÁLADU"); heading(c, "Barvy v pohybu.")
        c.addView(label("Od klidného večera po párty. Tvoje scény běží v lampě i po odpojení telefonu.", 15f, muted))
        c.addView(button("Nová scéna", "plus", true) { sceneEditor(null) })
        c.addView(button("Zastavit scénu", "stop") { call("scene/stop") { toast("Scéna zastavena."); scheduleRefresh() } })
        sceneList = column(); c.addView(sceneList); renderScenes(); readSceneDetails()
    }
    private fun readSceneDetails() {
        val ids = scenes.keys.toList(); val token = overviewToken
        fun next(index: Int) {
            if (tab != "scenes" || token != overviewToken || index >= ids.size) return
            call("get_scene", json("id" to ids[index])) { scene ->
                if (token == overviewToken && ids[index] in scenes) { scenes[ids[index]] = scene; renderScenes(); next(index+1) }
            }
        }
        next(0)
    }
    private fun renderScenes() {
        val list = sceneList ?: return; list.removeAllViews()
        scenes.forEach { (id, s) ->
            val sceneCard = column().apply { setPadding(dp(16), dp(18), dp(16), dp(14)) }
            val palette = s.optJSONArray("hues")
            val firstColor = if (palette != null && palette.length() > 0) Color.HSVToColor(floatArrayOf(palette.optInt(0).toFloat(), 1f, 1f)) else mint
            val lastColor = if (palette != null && palette.length() > 1) Color.HSVToColor(floatArrayOf(palette.optInt(palette.length()-1).toFloat(), 1f, 1f)) else Color.rgb(101,226,224)
            sceneCard.background = artwork(firstColor, lastColor)
            val dots = LinearLayout(this)
            if (palette != null) for (i in 0 until palette.length()) dots.addView(View(this).apply { background = artwork(Color.HSVToColor(floatArrayOf(palette.optInt(i).toFloat(), 1f, 1f)), orb = true) },
                LinearLayout.LayoutParams(if (palette.length() >= 6) 0 else dp(42), dp(42), if (palette.length() >= 6) 1f else 0f).apply { marginEnd = dp(2) })
            else dots.addView(label("✦", 26f, mint))
            sceneCard.addView(dots); sceneCard.addView(label(s.optString("name"), 21f).apply { typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL) })
            sceneCard.addView(label(if (s.has("mode")) modes.getOrElse(s.optInt("mode")) { "Barevná scéna" } else "Uložená scéna", 13f, muted))
            sceneCard.addView(row(button("Spustit", "play", true) { call("scene/run", json("id" to id)) { toast(if (demo != null) "Ukázka spuštění scény." else "Scéna běží v lampě.") } }, button("Upravit", "settings") { call("get_scene", json("id" to id)) { sceneEditor(it) } }))
            list.addView(sceneCard, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(16) })
        }
        if (scenes.isEmpty()) list.addView(label("Ještě tu není žádná scéna.", 15f, muted))
    }
    private fun edit(parent: LinearLayout, title: String, value: String, numeric: Boolean = false): EditText {
        parent.addView(label(title, 14f, muted))
        return EditText(this).apply {
            setText(value); setTextColor(ink); setSingleLine(true); contentDescription = title
            background = rounded(Color.rgb(24, 33, 35), 14).apply { setStroke(dp(1), Color.rgb(65, 83, 74)) }; setPadding(dp(14), dp(10), dp(14), dp(10)); setHintTextColor(muted)
            inputType = if (numeric) InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL else InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_SENTENCES
            parent.addView(this, LinearLayout.LayoutParams(-1, dp(52)))
        }
    }
    private fun form(title: String, content: LinearLayout, save: (() -> Unit)? = null): AlertDialog {
        content.setPadding(dp(20), dp(10), dp(20), dp(16))
        val dialog = AlertDialog.Builder(this).setTitle(title).setView(ScrollView(this).apply { addView(content) }).setNegativeButton("Zavřít", null)
            .apply { if (save != null) setPositiveButton("Uložit", null) }.create()
        dialog.setOnShowListener { if (save != null) dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener { try { save() } catch (e: Exception) { error(e) } } }
        dialog.show(); dialog.window?.setBackgroundDrawable(rounded(panelColor)); return dialog
    }
    private fun checkedLights(parent: LinearLayout, selectedIds: Set<String>): List<Pair<String, CheckBox>> {
        return lights.map { (id, l) -> id to CheckBox(this).apply { text = l.optString("name"); minHeight = dp(48); isChecked = id in selectedIds; parent.addView(this) } }
    }
    private fun name(field: EditText): String = field.text.toString().trim().also { require(it.isNotEmpty() && it.toByteArray().size <= 32 && it.none { c -> c.code < 32 }) { "Název musí mít 1–32 bajtů (české znaky mohou zabrat dva)." } }
    private fun groupList() {
        val c = column(); lateinit var dialog: AlertDialog
        c.addView(button("Nová skupina") { groupEditor(null) })
        groups.forEach { (id, g) -> c.addView(button(g.optString("name")) { call("get_group", json("id" to id)) { groupEditor(it) }; dialog.dismiss() }) }
        dialog = form("Skupiny světel", c)
    }
    private fun groupEditor(group: JSONObject?) {
        val c = column(); val title = edit(c, "Název skupiny", group?.optString("name") ?: "")
        val members = group?.optJSONArray("members")?.let { a -> (0 until a.length()).map { a.getString(it) }.toSet() } ?: emptySet()
        val checks = checkedLights(c, members); lateinit var dialog: AlertDialog
        if (group != null) c.addView(button("Smazat skupinu") { confirm("Smazat skupinu ${group.optString("name")}? Žárovky zůstanou spárované.") { mutate("group", json("id" to group.getString("id"), "delete" to true)) { dialog.dismiss() } } })
        dialog = form("Skupina", c) {
            val ids = checks.filter { it.second.isChecked }.map { it.first }; require(ids.isNotEmpty()) { "Vyber alespoň jedno světlo." }
            val args = json("name" to name(title), "members" to JSONArray(ids)); group?.let { args.put("id", it.getString("id")) }
            mutate("group", args) { dialog.dismiss() }
        }
    }
    private fun sceneEditor(scene: JSONObject?) {
        val c = column(); val title = edit(c, "Název", scene?.optString("name") ?: "Nová scéna")
        c.addView(label("Režim", 14f, muted)); val mode = spinner(modes); mode.setSelection(scene?.optInt("mode") ?: 12); c.addView(mode)
        val brightness = edit(c, "Jas (%)", percent(scene?.optInt("level") ?: 127).toString(), true)
        val hold = edit(c, "Výdrž barvy (s, 0–60)", ((scene?.optInt("hold") ?: 0)/10.0).toString(), true)
        val fade = edit(c, "Přechod (s, 0–10)", ((scene?.optInt("fade") ?: 30)/10.0).toString(), true)
        val hues = edit(c, "Vlastní paleta · odstíny 0–359, oddělené čárkou", scene?.optJSONArray("hues")?.let { a -> (0 until a.length()).joinToString(", ") { a.getInt(it).toString() } } ?: "240, 320")
        c.addView(button("Vybrat barvy na barevném kole") { paletteEditor(hues) })
        c.addView(label("0 červená · 60 žlutá · 120 zelená · 180 tyrkysová · 240 modrá · 320 růžová. U vlastní palety 2–8 barev; ostatní režimy mají vlastní palety.", 13f, muted))
        val useIr = CheckBox(this).apply { text = "Použít světla IR ovladače"; isChecked = scene?.optBoolean("useIr") ?: false; minHeight = dp(48) }; c.addView(useIr)
        val stagger = CheckBox(this).apply { text = "IR světla začnou postupně různými barvami"; isChecked = scene?.optBoolean("stagger") ?: false; minHeight = dp(48) }; c.addView(stagger)
        val memberBox = column(); c.addView(memberBox); memberBox.addView(label("Světla a počáteční krok (1–32)", 15f))
        val existing = scene?.optJSONArray("members")
        val offsets = mutableMapOf<String, Int>(); if (existing != null) for (i in 0 until existing.length()) { val a = existing.getJSONArray(i); offsets[a.getString(0)] = a.getInt(1) }
        val memberRows = column(); memberBox.addView(memberRows)
        data class Member(val id: String, val check: CheckBox, val step: EditText)
        val order = (offsets.keys + lights.keys).distinct().filter { it in lights }.map { id ->
            Member(id, CheckBox(this).apply { text = lights[id]!!.optString("name"); isChecked = if (scene == null) id != "display" else id in offsets; minHeight = dp(48) },
                EditText(this).apply { inputType = InputType.TYPE_CLASS_NUMBER; setText(((offsets[id] ?: 0)+1).toString()); contentDescription = "Počáteční krok ${lights[id]!!.optString("name")}" })
        }.toMutableList()
        fun redrawMembers() {
            memberRows.removeAllViews()
            order.forEachIndexed { i, m ->
                (m.check.parent as? ViewGroup)?.removeView(m.check); (m.step.parent as? ViewGroup)?.removeView(m.step)
                val line = LinearLayout(this)
                line.addView(m.check, LinearLayout.LayoutParams(0, -2, 1f))
                line.addView(m.step, LinearLayout.LayoutParams(dp(52), dp(52)))
                line.addView(button("↑") { if (i > 0) { order.removeAt(i); order.add(i-1, m); redrawMembers() } }.apply { contentDescription = "Posunout ${m.check.text} výše" }, LinearLayout.LayoutParams(dp(56), dp(52)))
                memberRows.addView(line)
            }
        }
        redrawMembers(); memberBox.visibility = if (useIr.isChecked) View.GONE else View.VISIBLE; stagger.visibility = if (useIr.isChecked) View.VISIBLE else View.GONE
        useIr.setOnCheckedChangeListener { _, enabled -> memberBox.visibility = if (enabled) View.GONE else View.VISIBLE; stagger.visibility = if (enabled) View.VISIBLE else View.GONE }
        lateinit var dialog: AlertDialog
        if (scene != null) c.addView(button("Smazat scénu") { confirm("Smazat scénu ${scene.optString("name")}? Zruší se také její přiřazení na IR ovladači.") { mutate("scenes", json("id" to scene.getString("id"), "delete" to true)) { dialog.dismiss() } } })
        dialog = form("Scéna", c) {
            val level = brightness.text.toString().toIntOrNull(); require(level != null && level in 1..100) { "Jas musí být 1–100 %." }
            fun time(input: EditText, max: Int): Int { val value = input.text.toString().replace(',', '.').toDoubleOrNull(); require(value != null && value.isFinite() && value >= 0 && value <= max) { "Neplatná délka přechodu nebo výdrže." }; return (value*10).roundToInt() }
            val colors = hues.text.toString().split(',').map { it.trim().toIntOrNull() }
            require(colors.size in 2..8 && colors.all { it != null && it in 0..359 }) { "Zadej 2–8 odstínů od 0 do 359." }
            val members = JSONArray(); order.filter { it.check.isChecked }.forEach { m -> val n = m.step.text.toString().toIntOrNull(); require(n != null && n in 1..32) { "Počáteční krok musí být 1–32." }; members.put(JSONArray(listOf(m.id, n-1))) }
            require(useIr.isChecked || members.length() > 0) { "Vyber světla pro scénu." }
            val holdDs = time(hold, 60); val fadeDs = time(fade, 10); require(holdDs+fadeDs >= 5) { "Cyklus musí mít alespoň 0,5 sekundy." }
            val args = json("name" to name(title), "mode" to mode.selectedItemPosition, "level" to (level*254f/100).roundToInt().coerceIn(1, 254), "hold" to holdDs, "fade" to fadeDs,
                "hues" to JSONArray(colors), "members" to members, "useIr" to useIr.isChecked, "stagger" to stagger.isChecked)
            scene?.let { args.put("id", it.getString("id")) }; mutate("scenes", args) { dialog.dismiss() }
        }
    }
    private fun paletteEditor(field: EditText) {
        val palette = field.text.toString().split(',').mapNotNull { it.trim().toIntOrNull()?.takeIf { n -> n in 0..359 } }.take(8).toMutableList()
        if (palette.size < 2) { palette.clear(); palette.addAll(listOf(240, 320)) }
        val c = column(); var index = 0
        c.addView(label("Vyber pořadí barvy a pak odstín. Scény používají plnou sytost; přechod vede po barevném kruhu.", 14f, muted))
        val pick = spinner(palette.indices.map { "Barva ${it+1}" }); c.addView(pick)
        val preview = label("", 20f); preview.gravity = Gravity.CENTER; preview.setPadding(0, dp(16), 0, dp(16)); c.addView(preview)
        val wheel = ColorWheel(this).apply { hue = palette[0].toFloat(); saturation = 100f }; c.addView(wheel)
        val hue = LightSlider(this, "hue").apply { max = 359; contentDescription = "Odstín barvy v paletě" }; c.addView(hue)
        fun render() {
            val value = palette[index]; wheel.hue = value.toFloat(); wheel.saturation = 100f; wheel.invalidate(); hue.progress = value
            preview.text = "${index+1}. barva · $value°"; preview.background = rounded(Color.HSVToColor(floatArrayOf(value.toFloat(), 1f, .5f)), 12)
        }
        fun list() { pick.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, palette.indices.map { "Barva ${it+1}" }); pick.setSelection(index); render() }
        pick.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onNothingSelected(parent: AdapterView<*>?) {}
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) { index = position.coerceIn(palette.indices); render() }
        }
        wheel.onColor = { h, _, _ -> palette[index] = h; render() }
        hue.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onStartTrackingTouch(s: SeekBar) {}
            override fun onStopTrackingTouch(s: SeekBar) {}
            override fun onProgressChanged(s: SeekBar, value: Int, user: Boolean) { if (user) { palette[index] = value; render() } }
        })
        c.addView(row(button("Přidat barvu") { if (palette.size < 8) { palette.add((palette.last()+60)%360); index = palette.lastIndex; list() } else toast("Paleta má nejvýše osm barev.") },
            button("Odebrat barvu") { if (palette.size > 2) { palette.removeAt(index); index = minOf(index, palette.lastIndex); list() } else toast("Ponech alespoň dvě barvy.") }))
        render(); lateinit var dialog: AlertDialog
        dialog = form("Barvy vlastní palety", c) { field.setText(palette.joinToString(", ")); dialog.dismiss() }
    }
    private fun confirm(message: String, action: () -> Unit) { AlertDialog.Builder(this).setMessage(message).setNegativeButton("Zrušit", null).setPositiveButton("Potvrdit") { _, _ -> action() }.show() }
    private fun lightSettings(id: String) {
        readLight(id) { light ->
            val c = column(); val title = edit(c, "Název světla", light.optString("name")); lateinit var dialog: AlertDialog
            c.addView(button("Přejmenovat") { try { mutate("rename", json("id" to id, "name" to name(title))) } catch (e: Exception) { error(e) } })
            if (id != "display") {
                c.addView(label("${light.optString("manufacturer")} ${light.optString("model")}\n$id", 13f, muted))
                if (light.optBoolean("removing")) c.addView(label("Čekám na potvrzení odpárování od žárovky.", 14f))
                val powerStates = listOf("Napájecí stav se nemění.", "Čekám na zápis a ověření napájecího stavu.", "Napájecí stav potvrzen žárovkou.", "Napájecí stav není potvrzen; některé hodnoty se mohly zapsat.", "Napájecí stav čeká na připojení žárovky.")
                c.addView(label(powerStates[light.optInt("powerStatus").coerceIn(0, 4)], 13f, muted))
                for (key in listOf("removeError", "powerError")) if (light.optString(key).isNotEmpty()) c.addView(label("Stav žárovky: ${light.optString(key)}", 14f))
                c.addView(button("Identifikovat žárovku") { call("identify", json("id" to id)) { toast("Identifikace odeslána.") } })
                val transition = edit(c, "Přechod příkazů (s, 0–10)", (light.optInt("transition", 3)/10.0).toString(), true)
                val legacy = CheckBox(this).apply { text = "Kompatibilita: používat XY místo Hue/Saturation"; isChecked = light.optBoolean("legacyXy"); minHeight = dp(48) }; c.addView(legacy)
                c.addView(button("Uložit přechod a barvy") {
                    val t = transition.text.toString().replace(',', '.').toDoubleOrNull()
                    if (t == null || !t.isFinite() || t !in 0.0..10.0) toast("Přechod musí být 0–10 s.")
                    else mutate("device", json("id" to id, "transition" to (t*10).roundToInt(), "legacyXy" to legacy.isChecked))
                })
                c.addView(button("Chování po zapnutí") { startupEditor(id, light) })
                c.addView(button("Odpárovat žárovku") { confirm("Odpárovat ${light.optString("name")}? Žárovka musí být dostupná, aby potvrdila odchod ze Zigbee sítě.") { mutate("remove", json("id" to id, "force" to false)) { dialog.dismiss() } } })
                c.addView(button("Force remove") { confirm("Vynutit odebrání ${light.optString("name")}? Odstraní se z evidence, skupin a scén i bez potvrzení. Před dalším párováním může být nutný reset žárovky.") { mutate("remove", json("id" to id, "force" to true)) { dialog.dismiss() } } })
            } else c.addView(label("Barevná plocha displeje. Probuzení a noční časovač nastavíš ve webu; BLE ho samo neprobudí.", 14f, muted))
            dialog = form("Nastavení světla", c)
        }
    }
    private fun startupEditor(id: String, light: JSONObject) {
        val c = column(); val config = light.optJSONObject("startup") ?: JSONObject(); val wanted = light.optJSONObject("wanted") ?: defaults()
        c.addView(label("Po příkazu Rozsvítit", 15f)); val wake = spinner(listOf("Paměť posledního stavu", "Vlastní preset")); wake.setSelection(config.optInt("wakeMode")); c.addView(wake)
        val wakePreset = JSONObject((config.optJSONObject("wake") ?: wanted).toString()).put("on", true)
        val powerPreset = JSONObject((config.optJSONObject("power") ?: wanted).toString()).put("on", true)
        c.addView(button("Preset probuzení = poslední požadovaný stav světla") { wanted.keys().forEach { wakePreset.put(it, wanted.get(it)) }; wakePreset.put("on", true); wake.setSelection(1); toast("Zachycen stav tohoto světla při otevření nastavení.") })
        c.addView(label("Po přivedení napájení", 15f))
        val power = spinner(listOf("Ponechat chování výrobce", "Zůstat vypnutá", "Obnovit stav i zapnutí", "Zapnout s poslední barvou", "Zapnout do presetu")); power.setSelection(config.optInt("powerMode")); c.addView(power)
        c.addView(button("Napájecí preset = poslední požadovaný stav světla") { wanted.keys().forEach { powerPreset.put(it, wanted.get(it)) }; powerPreset.put("on", true); power.setSelection(4); toast("Zachycen stav tohoto světla při otevření nastavení.") })
        c.addView(label("Požadovaný stav nastav předem posuvníky u této konkrétní žárovky. Výchozí preset se zachová, dokud jej nezachytíš znovu. Skutečná podpora napájecího stavu závisí na žárovce.", 13f, muted))
        var dialog: AlertDialog? = null
        dialog = form("Probuzení a napájení", c) { mutate("startup", json("id" to id, "wakeMode" to wake.selectedItemPosition, "powerMode" to power.selectedItemPosition, "wake" to wakePreset, "power" to powerPreset)) { dialog?.dismiss() } }
    }
    private fun readNetwork() { call("get_network_status") {
        network = it
        val address = if (it.optBoolean("apActive")) "192.168.4.1" else if (it.optBoolean("staConnected")) it.optString("staIp") else ""
        if (demo == null && address.isNotBlank()) runCatching { prefs.edit().putString("webAddress", pairingUri(address).buildUpon().fragment(null).build().toString()).apply() }
        if (tab == "settings") { val y = scroll.scrollY; show("settings"); scroll.post { scroll.scrollTo(0, y) } }
    } }
    private fun remoteEditor() {
        call("get_remote") { config ->
            val c = column()
            c.addView(label(if (config.optBoolean("irEnabled")) "IR přijímač je zapnutý." else "IR přijímač je vypnutý; zapíná se ve webu.", 14f, muted))
            c.addView(label("Žárovky ovládané ovladačem", 20f))
            val members = config.optJSONArray("irMembers") ?: JSONArray()
            val checks = checkedLights(c, (0 until members.length()).map { members.getString(it) }.toSet())
            c.addView(label("Prázdný výběr použije cíl nastavený ve webu: ${config.optString("irTarget", "all")}.", 13f, muted))
            val kelvin = edit(c, "Tlačítko bílá · teplota (K)", config.optInt("whiteKelvin", 2700).toString(), true)
            val white = edit(c, "Tlačítko bílá · jas (%)", percent(config.optInt("whiteLevel", 127)).toString(), true)
            c.addView(label("Kroky jasu", 20f))
            val custom = CheckBox(this).apply { text = "Vlastní jasy jednotlivých kroků"; isChecked = config.optBoolean("customBrightness"); minHeight = dp(48) }; c.addView(custom)
            val count = edit(c, "Počet automatických kroků (2–32)", config.optInt("brightnessSteps", 10).toString(), true)
            val saved = config.optJSONArray("brightnessLevels") ?: JSONArray(listOf(1,2,4,8,15,25,40,60,80,100))
            val levels = edit(c, "Vlastní kroky v % · oddělené čárkou", (0 until saved.length()).joinToString(", ") { saved.getInt(it).toString() })
            fun toggle() { count.isEnabled = !custom.isChecked; levels.isEnabled = custom.isChecked }
            custom.setOnCheckedChangeListener { _, _ -> toggle() }; toggle()
            c.addView(label("Automatická škála začíná na 1 % a končí na 100 %. Vlastní škála má 2–32 rostoucích celých hodnot mezi 1 a 100 %; počet kroků určí seznam.", 13f, muted))
            c.addView(label("Tlačítka scén", 20f))
            val choices = listOf("" to "Bez přiřazení") + scenes.map { it.key to it.value.optString("name") }
            val buttons = config.optJSONArray("buttons") ?: JSONArray()
            val selects = listOf("FLASH", "STROBE", "FADE / MODE", "SMOOTH").mapIndexed { i, title ->
                c.addView(label(title, 14f, muted)); spinner(choices.map { it.second }).also { s ->
                    s.setSelection(choices.indexOfFirst { it.first == buttons.optString(i, "") }.coerceAtLeast(0)); c.addView(s)
                }
            }
            c.addView(label("Scéna s vlastním výběrem světel řídí své členy. Scéna s volbou „Použít světla IR ovladače“ použije výběr nahoře.", 13f, muted))
            lateinit var dialog: AlertDialog
            dialog = form("IR ovladač", c) {
                val k = kelvin.text.toString().toIntOrNull(); require(k != null && k in 2000..6500) { "Teplota musí být 2000–6500 K." }
                val w = white.text.toString().toIntOrNull(); require(w != null && w in 1..100) { "Jas bílé musí být 1–100 %." }
                val args = json("whiteKelvin" to k, "whiteLevel" to (w*254f/100).roundToInt(), "customBrightness" to custom.isChecked,
                    "irMembers" to JSONArray(checks.filter { it.second.isChecked }.map { it.first }),
                    "buttons" to JSONArray(selects.map { choices[it.selectedItemPosition].first.ifEmpty { null } }))
                if (custom.isChecked) {
                    val parsed = levels.text.toString().split(',').map { it.trim().toIntOrNull() }
                    require(parsed.size in 2..32 && parsed.all { it != null && it in 1..100 } && parsed.zipWithNext().all { (a,b) -> a!! < b!! }) { "Zadej 2–32 rostoucích jasů od 1 do 100 %." }
                    args.put("brightnessSteps", parsed.size).put("brightnessLevels", JSONArray(parsed))
                } else {
                    val n = count.text.toString().toIntOrNull(); require(n != null && n in 2..32) { "Počet kroků musí být 2–32." }; args.put("brightnessSteps", n)
                }
                mutate("remote", args) { dialog.dismiss() }
            }
        }
    }
    private fun timerSettings() {
        val c = column(); c.addView(label("Odpočet běží v lampě. Telefon i web můžeš odpojit.", 18f))
        val items = targetItems().filter { it.first != "display" }
        val target = spinner(items.map { it.second }); target.setSelection(items.indexOfFirst { it.first == selected }.coerceAtLeast(0)); c.addView(target)
        val minutes = edit(c, "Vypnout za (minuty)", "60", true)
        c.addView(label("1–1440 minut. Nastavení časovače samo nemění jas ani nerozsvěcí. Změna barvy či jasu odpočet zachová; ruční vypnutí jej zruší. Při restartu Wi-Fi desky se čekající vypnutí provede hned po obnovení spojení se žárovkou.", 14f, muted))
        val running = label("Načítám časovače…", 15f, mint); c.addView(running)
        fun loadTimers() {
            call("get_timers") { result ->
                val rows = result.optJSONArray("items") ?: JSONArray()
                running.text = if (rows.length() == 0) "Žádný časovač neběží." else (0 until rows.length()).joinToString("\n") { i ->
                    val t = rows.getJSONObject(i); val seconds = t.optInt("seconds")
                    "${lights[t.optString("id")]?.optString("name") ?: t.optString("id")}: ${if (seconds > 0) "${seconds / 60} min ${seconds % 60} s" else "čeká na potvrzení vypnutí"}"
                }
            }
        }
        c.addView(button("Nastavit časovač", "bulb", true) {
            val value = minutes.text.toString().toIntOrNull()
            if (value == null || value !in 1..1440) { minutes.error = "Zadej 1–1440 minut."; return@button }
            call("timer", json("target" to items[target.selectedItemPosition].first, "seconds" to value * 60)) { toast("Časovač uložen v lampě."); loadTimers() }
        })
        c.addView(button("Zrušit časovač výběru") { call("timer", json("target" to items[target.selectedItemPosition].first, "seconds" to 0)) { loadTimers() } })
        c.addView(button("Obnovit zbývající čas") { loadTimers() })
        form("Časovač vypnutí", c); loadTimers()
    }
    private fun alarmAccess(): Boolean {
        val component = ComponentName(this, AlarmLightService::class.java)
        return if (Build.VERSION.SDK_INT >= 27) getSystemService(NotificationManager::class.java).isNotificationListenerAccessGranted(component)
        else Settings.Secure.getString(contentResolver, "enabled_notification_listeners").orEmpty().split(':').any { ComponentName.unflattenFromString(it) == component }
    }
    private fun alarmSettings() {
        val p = app.alarmPrefs; val c = column(); MorningAlarm.observe(this)
        eyebrow(c, "RÁNO PODLE TVÉHO BUDÍKU")
        c.addView(label("Budík zazvoní. Světlo se přidá.", 23f))
        val enabled = Switch(this).apply { text = "Ranní světlo s budíkem"; minHeight = dp(56); isChecked = p.getBoolean("enabled", false); setTextColor(ink) }; c.addView(enabled)
        c.addView(label("Hodiny Samsung · reaguje na skutečné oznámení zvonění a ověřuje čas budíku. Jednou za den; opakované zvonění ani odložení neprodlužuje svícení. Začni krátkým zkušebním budíkem v nastaveném rozmezí.", 14f, muted))
        val start = edit(c, "Ráno od (HH:mm)", ArrivalSchedule.format(p.getInt("start", 300)))
        val end = edit(c, "Ráno do (HH:mm)", ArrivalSchedule.format(p.getInt("end", 600)))
        val brightness = edit(c, "Jas při budíku (%)", p.getInt("brightness", 10).toString(), true)
        val minutes = edit(c, "Vypnout po budíku za (minuty)", p.getInt("minutes", 60).toString(), true)
        val stored = p.getString("target", "all") ?: "all"
        val items = targetItems().filter { it.first != "display" }.toMutableList()
        if (items.none { it.first == stored }) items.add(stored to p.getString("targetName", stored).orEmpty())
        c.addView(label("Co rozsvítit", 14f, muted)); val target = spinner(items.map { it.second }); target.setSelection(items.indexOfFirst { it.first == stored }); c.addView(target)
        c.addView(label("Barva se vezme z paměti / presetu žárovky, jas se nastaví podle budíku. Pro vlastní výběr žárovek si vytvoř skupinu. Odpočet vypnutí po odeslání běží ve Wi-Fi desce, i když telefon odpojíš.", 14f, muted))
        c.addView(button("Povolit přístup k oznámením", "settings") {
            startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        })
        val status = label("", 14f, mint); c.addView(status)
        c.addView(label("Používají se jen oznámení Hodin Samsung; text oznámení se neukládá. V nastavení baterie Z-lamp nastav Bez omezení. Pokud je lampa nedostupná, ranní rozsvícení se po minutě vzdá a později se neopakuje.", 14f, muted))
        c.addView(button("Nastavení aplikace a baterie", "settings") { startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:$packageName"))) })
        val dialog = form("Ranní světlo s budíkem", c) {
            val from = ArrivalSchedule.parse(start.text.toString()); val to = ArrivalSchedule.parse(end.text.toString())
            val percent = brightness.text.toString().toIntOrNull(); val duration = minutes.text.toString().toIntOrNull()
            require(from != to) { "Zadej různé časy ve formátu HH:mm." }
            require(percent != null && percent in 1..100 && duration != null && duration in 1..1440) { "Jas 1–100 %, čas 1–1440 minut." }
            if (enabled.isChecked) {
                require(demo == null && client.savedAddress.isNotBlank() && ArrivalService.permitted(this) && client.isBonded(client.savedAddress)) { "Nejdřív připoj skutečnou lampu a povol Bluetooth." }
                require(alarmAccess()) { "Nejdřív povol Z-lamp přístup k oznámením." }
                require(!client.ready || client.info.optInt("timers") >= 1) { "Aktualizuj Wi-Fi firmware na 2.10.0." }
            }
            val choice = items[target.selectedItemPosition]
            p.edit().putBoolean("enabled", enabled.isChecked).putString("clockPackage", MorningAlarm.SAMSUNG)
                .putInt("start", from).putInt("end", to).putInt("brightness", percent).putInt("minutes", duration)
                .putString("target", choice.first).putString("targetName", choice.second).putString("address", client.savedAddress).apply()
            android.service.notification.NotificationListenerService.requestRebind(ComponentName(this, AlarmLightService::class.java))
            toast("Nastavení budíku uloženo.")
        }
        val update = object : Runnable {
            override fun run() {
                if (!dialog.isShowing) return
                val time = p.getLong("nextAt", 0)
                val next = if (time > System.currentTimeMillis() && p.getString("nextPackage", "") == MorningAlarm.SAMSUNG) java.text.DateFormat.getDateTimeInstance(java.text.DateFormat.SHORT, java.text.DateFormat.SHORT).format(java.util.Date(time)) else "Hodiny zatím nehlásí další budík"
                status.text = "Přístup k oznámením: ${if (alarmAccess()) "povolen" else "čeká na povolení"}\n$next\n${p.getString("lastResult", "Zatím žádné ranní zvonění.")}\n${p.getString("diagnostic", "")}"
                handler.postDelayed(this, 1000)
            }
        }
        handler.post(update); dialog.setOnDismissListener { handler.removeCallbacks(update) }
    }
    private fun nightSummary(): String {
        val p = app.arrivalPrefs
        return if (!p.getBoolean("nightEnabled", true)) "Časový režim vypnutý · použít paměť světel"
        else "${ArrivalSchedule.format(p.getInt("evening", 1260))} → ${ArrivalSchedule.format(p.getInt("night", 1380))} · v noci ${p.getInt("nightPercent", 1)} % / ${p.getInt("nightMinutes", 5)} min"
    }
    private fun nightSettings(saved: () -> Unit) {
        val p = app.arrivalPrefs; val c = column()
        eyebrow(c, "TICHÝ NÁVRAT DOMŮ")
        c.addView(label("Večer jemně. V noci jen na cestu.", 22f))
        val enabled = Switch(this).apply { text = "Přizpůsobit příchod času telefonu"; isChecked = p.getBoolean("nightEnabled", true); minHeight = dp(56); setTextColor(ink) }; c.addView(enabled)
        val evening = edit(c, "Večer od (HH:mm)", ArrivalSchedule.format(p.getInt("evening", 1260)))
        val night = edit(c, "Noční režim od (HH:mm)", ArrivalSchedule.format(p.getInt("night", 1380)))
        val morning = edit(c, "Denní režim od (HH:mm)", ArrivalSchedule.format(p.getInt("morning", 420)))
        val dayLevel = edit(c, "Denní jas / začátek večera (%)", p.getInt("dayPercent", 100).toString(), true)
        val nightLevel = edit(c, "Noční jas (%)", p.getInt("nightPercent", 1).toString(), true)
        c.addView(label("Jas mezi večerem a nocí klesá logaritmicky. Výchozí: 21:00 → 100 %, 22:00 → 10 %, 23:00 až 7:00 → 1 %. Platí pro nový příchod; světla, která už svítí, se nemění.", 14f, muted))
        val id = p.getString("nightTarget", "").orEmpty()
        val choices = mutableListOf("" to "Vyber jednu žárovku")
        if (demo == null) choices.addAll(lights.filterKeys { it != "display" }.map { it.key to it.value.optString("name") })
        if (id.isNotEmpty() && choices.none { it.first == id }) choices.add(id to (p.getString("nightName", id) ?: id))
        c.addView(label("Jediná noční žárovka", 14f, muted)); val bulb = spinner(choices.map { it.second }); bulb.setSelection(choices.indexOfFirst { it.first == id }.coerceAtLeast(0)); c.addView(bulb)
        val minutes = edit(c, "V noci zhasnout za (1–60 min)", p.getInt("nightMinutes", 5).toString(), true)
        val kelvin = edit(c, "Noční bílá (2000–6500 K)", p.getInt("nightKelvin", 2700).toString(), true)
        c.addView(label("Ve dne a večer se použije výběr světel z Příchodu a barva jejich probuzení. V noci se zapne pouze vybraná žárovka s nastavenou bílou. Odpočet běží v lampě i bez telefonu. Ruční změna dané žárovky přes aplikaci, web nebo IR odpočet zruší.", 14f, muted))
        c.addView(label("Vyžaduje firmware Wi-Fi 2.9.0 a C6 2.7.1. Aktuálně: ${client.info.optString("version", "nepřipojeno")}. Čas a časové pásmo se berou z telefonu při příchodu; nejde o nepřetržité stmívání už zapnutých světel.", 13f, muted))
        lateinit var dialog: AlertDialog
        dialog = form("Večer a noc", c) {
            val day = dayLevel.text.toString().toIntOrNull(); val low = nightLevel.text.toString().toIntOrNull()
            require(day != null && low != null) { "Zadej jas v celých procentech." }
            val schedule = ArrivalSchedule(ArrivalSchedule.parse(evening.text.toString()), ArrivalSchedule.parse(night.text.toString()), ArrivalSchedule.parse(morning.text.toString()), day, low)
            val length = minutes.text.toString().toIntOrNull(); val temperature = kelvin.text.toString().toIntOrNull()
            require(length != null && length in 1..60 && temperature != null && temperature in 2000..6500) { "Zadej 1–60 minut a 2000–6500 K." }
            val choice = choices[bulb.selectedItemPosition]
            require(!enabled.isChecked || Regex("[0-9a-fA-F]{16}").matches(choice.first)) { "Vyber jednu konkrétní žárovku pro noc." }
            require(demo == null) { "Noční režim nastav u skutečné lampy." }
            p.edit().putBoolean("nightEnabled", enabled.isChecked).putInt("evening", schedule.evening).putInt("night", schedule.night)
                .putInt("morning", schedule.morning).putInt("dayPercent", day).putInt("nightPercent", low)
                .putInt("nightMinutes", length).putInt("nightKelvin", temperature).putString("nightTarget", choice.first).putString("nightName", choice.second).apply()
            app.arrival.resetSignal(); dialog.dismiss(); saved()
        }
    }
    private fun arrivalSettings() {
        if (arrivalDialog?.isShowing == true) return
        val p = app.arrivalPrefs
        val c = column()
        eyebrow(c, "DOMA JE TAM, KDE SE ROZSVÍTÍ")
        c.addView(label("Připojit → přiblížit → rozsvítit jednou.", 20f))
        c.addView(label("Měří se signál k desce s displejem. Po rozsvícení se další příchod povolí až po odpojení. Odchod světla nezhasíná.", 14f, muted))
        val live = label("Čekám na signál…", 25f, mint).apply { typeface = Typeface.MONOSPACE }
        val state = label("", 14f, muted)
        c.addView(live); c.addView(state)
        val enabled = Switch(this).apply { text = "Automatický příchod"; minHeight = dp(56); isChecked = p.getBoolean("enabled", false); setTextColor(ink) }
        c.addView(enabled)
        val thresholdText = label("", 19f); c.addView(thresholdText)
        val threshold = LightSlider(this, "level").apply {
            max = 65; progress = (p.getInt("threshold", -60) + 95).coerceIn(0, 65); contentDescription = "Práh signálu v dBm"
        }
        fun thresholdLabel() { thresholdText.text = "Rozsvítit od ${threshold.progress - 95} dBm" }
        threshold.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onStartTrackingTouch(s: SeekBar) {}
            override fun onStopTrackingTouch(s: SeekBar) {}
            override fun onProgressChanged(s: SeekBar, n: Int, user: Boolean) { thresholdLabel() }
        }); c.addView(threshold); thresholdLabel()
        c.addView(button("Použít aktuální signál jako práh") {
            val rssi = client.lastRssi
            if (demo == null && client.ready && rssi != null && SystemClock.elapsedRealtime() - client.rssiAt < 3500) {
                threshold.progress = (rssi + 95).coerceIn(0, 65)
            } else toast("Nejdřív připoj skutečnou lampu a počkej na měření.")
        })
        c.addView(label("−50 dBm je silnější než −70 dBm. Postav se k místu příchodu s telefonem tak, jak ho běžně nosíš. Zkus práh i z chodby. Při otevřeném nastavení se automaticky nerozsvěcí.", 13f, muted))
        val dwell = edit(c, "Potvrzení silného signálu (s; 0 = ihned)", p.getInt("dwell", 0).toString(), true)
        c.addView(label("0 = rozsvítit při prvním měření na prahu nebo nad ním. Vyšší počet sekund vyžaduje silný signál po celou tuto dobu; pokles pod práh čekání vynuluje.", 13f, muted))
        val grace = edit(c, "Minimální odpojení pro nový příchod (0–120 s)", p.getInt("grace", 0).toString(), true)
        c.addView(label("0 = každý nový spoj. Vyšší hodnota ignoruje krátké výpadky; 15–30 s může zabránit nechtěnému opakování.", 13f, muted))
        val storedTarget = p.getString("target", "all") ?: "all"
        val items = if (demo == null) targetItems().toMutableList() else mutableListOf("all" to "Všechny žárovky")
        if (items.none { it.first == storedTarget }) items.add(storedTarget to (p.getString("targetName", storedTarget) ?: storedTarget))
        c.addView(label("Co rozsvítit", 14f, muted))
        val target = spinner(items.map { it.second }); target.setSelection(items.indexOfFirst { it.first == storedTarget }); c.addView(target)
        c.addView(label("Vlastní výběr více světel si vytvoř jako skupinu. Bez časového režimu se použije paměť nebo preset probuzení každé žárovky.", 13f, muted))
        val summary = label(nightSummary(), 16f, mint); c.addView(summary)
        c.addView(button("Večerní a noční režim", "settings") { nightSettings { summary.text = nightSummary() } })
        val last = p.getString("lastResult", "").orEmpty()
        if (last.isNotEmpty()) c.addView(label("Poslední akce: $last", 13f, muted))
        c.addView(label("Povol oznámení a v nastavení baterie zvol pro Z-lamp režim Bez omezení. Funkce běží i při zhasnutém displeji telefonu; během čekání na přiblížení spotřebuje více energie. Android ji po Vynuceném ukončení obnoví až otevřením aplikace.", 13f, muted))
        c.addView(button("Nastavení aplikace a baterie", "settings") {
            startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:$packageName")))
        })
        c.addView(label("Ukládá se v tomto telefonu pro ${client.savedName}. Ruční odpojení, nové párování nebo ukázka Příchod vypnou.", 13f, muted))
        app.calibrating = true; app.arrival.resetSignal()
        val dialog = form("Automatický příchod", c) {
            val seconds = dwell.text.toString().toIntOrNull(); val away = grace.text.toString().toIntOrNull()
            require(seconds != null && seconds >= 0) { "Potvrzení nastav jako celé nezáporné sekundy; 0 znamená ihned." }
            require(away != null && away in 0..120) { "Minimální odpojení nastav od 0 do 120 sekund." }
            if (enabled.isChecked) {
                require(demo == null && client.savedAddress.isNotBlank()) { "Nejdřív připoj skutečnou lampu. V ukázce se Příchod nezapíná." }
                require(ArrivalService.permitted(this) && client.isBonded(client.savedAddress)) { "Nejdřív obnov oprávnění Bluetooth a párování lampy." }
                if (p.getBoolean("nightEnabled", true)) {
                    require(p.getString("nightTarget", "").orEmpty().isNotBlank()) { "Nejdřív v nabídce Večerní a noční režim vyber noční žárovku, nebo časový režim vypni." }
                    require(!client.ready || client.info.optInt("arrival") >= 1) { "Nejdřív aktualizuj Wi-Fi desku na 2.9.0 a C6 na 2.7.1; nebo časový režim vypni." }
                }
            }
            val choice = items[target.selectedItemPosition]
            p.edit().putInt("threshold", threshold.progress - 95).putInt("dwell", seconds).putInt("grace", away)
                .putString("target", choice.first).putString("targetName", choice.second).putString("address", client.savedAddress)
                .putBoolean("enabled", enabled.isChecked).apply()
            if (enabled.isChecked) {
                try { ArrivalService.start(this) } catch (e: RuntimeException) {
                    p.edit().putBoolean("enabled", false).apply(); throw IllegalStateException("Příchod se nepodařilo spustit. ${e.message}")
                }
                if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
                    requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 9)
            } else ArrivalService.stop(this)
            arrivalDialog?.dismiss(); if (tab == "settings") show("settings")
            toast(if (enabled.isChecked) "Příchod zapnutý. Práh si ověř při skutečném příchodu." else "Příchod vypnutý.")
        }
        arrivalDialog = dialog
        val sample = object : Runnable {
            override fun run() {
                if (!alive || !dialog.isShowing) return
                if (active && demo == null) {
                    client.readSignal()
                    val fresh = client.ready && SystemClock.elapsedRealtime() - client.rssiAt < 3500
                    live.text = if (fresh && client.lastRssi != null) "${client.lastRssi} dBm" else "— dBm"
                    state.text = when {
                        !client.ready -> "${client.status}\nPro kalibraci připoj lampu."
                        app.arrival.consumed -> "Tento příchod už je vyřízený. Další až po odpojení."
                        else -> "Kalibrace · automatické rozsvícení pozastaveno."
                    }
                } else if (demo != null) { live.text = "— dBm"; state.text = "Ukázka · měření vyžaduje skutečnou lampu." }
                handler.postDelayed(this, 1000)
            }
        }
        dialog.setOnDismissListener { handler.removeCallbacks(sample); app.calibrating = false; app.arrival.resetSignal(); arrivalDialog = null }
        handler.post(sample)
    }
    private fun tileSettings() {
        val p = app.tilePrefs; val c = column()
        eyebrow(c, "SVĚTLO NA JEDNO KLEPNUTÍ")
        c.addView(label("Stáhni lištu. Rozsviť.", 24f))
        c.addView(label("Dlaždice Z-lamp přepne světla i bez otevřené aplikace a nevyžaduje odemčení telefonu. Pokud aspoň jedno vybrané světlo svítí, zhasne výběr; jinak ho rozsvítí podle nastavení probuzení žárovek.", 15f, muted))
        val selected = p.getString("target", "all") ?: "all"
        val choices = mutableListOf("all" to "Všechny žárovky")
        if (demo == null) { choices.addAll(groups.map { it.key to it.value.optString("name") }); choices.addAll(lights.map { it.key to it.value.optString("name") }) }
        if (choices.none { it.first == selected }) choices.add(selected to (p.getString("name", selected) ?: selected))
        c.addView(label("Co ovládá dlaždice", 14f, muted))
        val target = spinner(choices.map { it.second }); target.setSelection(choices.indexOfFirst { it.first == selected }); c.addView(target)
        fun save() {
            require(demo == null) { "Dlaždici nastav u skutečné lampy." }
            require(!app.tileBusy) { "Počkej na dokončení příkazu z lišty." }
            val choice = choices[target.selectedItemPosition]
            p.edit().putString("target", choice.first).putString("name", choice.second).apply(); app.tileOn = null
        }
        c.addView(button("Přidat do horní lišty", "plus", true) {
            try {
                save()
                if (Build.VERSION.SDK_INT >= 33) getSystemService(StatusBarManager::class.java).requestAddTileService(
                    ComponentName(this, LightTileService::class.java), "Z-lamp", android.graphics.drawable.Icon.createWithResource(this, R.drawable.ic_tile_light), mainExecutor
                ) { result -> toast(when (result) {
                    StatusBarManager.TILE_ADD_REQUEST_RESULT_TILE_ADDED -> "Z-lamp je v rychlém nastavení."
                    StatusBarManager.TILE_ADD_REQUEST_RESULT_TILE_ALREADY_ADDED -> "Dlaždice už v liště je."
                    else -> "Stáhni lištu → Upravit → přetáhni dlaždici Z-lamp mezi aktivní."
                }) }
                else toast("Stáhni lištu dvakrát → Upravit (tužka) → přetáhni Z-lamp mezi aktivní dlaždice.")
            } catch (e: RuntimeException) { error(e) }
        })
        c.addView(label("Při prvním použití spáruj lampu v aplikaci a povol Bluetooth. Potom stačí klepnout na dlaždici, i když je Příchod vypnutý. Připojení může chvíli trvat; během příkazu je další klepnutí blokované.", 14f, muted))
        c.addView(label("Podržením dlaždice otevřeš toto nastavení. Dostupnost lišty na zamčené obrazovce určuje také systém telefonu. Po restartu telefon nejprve jednou odemkni. Ruční přepnutí z lišty zruší případný noční časovač vybraných světel a pro toto BLE spojení potlačí automatický příchod.", 14f, muted))
        lateinit var dialog: AlertDialog
        dialog = form("Světla v horní liště", c) { save(); dialog.dismiss() }
    }
    private fun companionSettings() {
        val content = column()
        content.addView(label("Android hlídá uloženou lampu a probudí aplikaci, když se objeví v dosahu. Obnovení spojení používá uložené klíče; samo nezapíná světla.", 15f, muted))
        content.addView(label(if (CompanionLink.enabled(this)) "Systémové připojování je zapnuté." else "Systémové připojování je vypnuté.", 16f))
        if (!CompanionLink.supported(this)) {
            content.addView(label("Tato funkce vyžaduje Android 12 nebo novější s podporou doprovodných zařízení. Na starším telefonu lze použít službu Automatický příchod.", 14f, muted))
        } else {
            content.addView(label("Jednou schválíš propojení v okně Androidu. Pro automatické rozsvícení zapni také Příchod. Vynucené zastavení aplikace v nastavení telefonu automatické připojování přeruší.", 14f, muted))
            content.addView(button("Zapnout systémové připojování", "bluetooth", true) { withBluetooth { enableCompanion() } })
            content.addView(button("Vypnout systémové připojování") { CompanionLink.disable(this); toast("Systémové připojování vypnuto. Párování zůstalo uložené.") })
        }
        content.addView(label("Uložená adresa: ${client.savedAddress.ifBlank { "žádná" }}\nPárování v Androidu: ${if (client.isBonded(client.savedAddress)) "uložené" else "není uložené"}", 13f, muted))
        form("Systémové připojování", content)
    }
    private fun enableCompanion() {
        if (Build.VERSION.SDK_INT < 31 || !CompanionLink.supported(this)) return
        val address = client.savedAddress
        if (demo != null || address.isBlank() || !client.isBonded(address)) { toast("Nejdřív připoj skutečnou lampu v aplikaci."); return }
        val manager = getSystemService(CompanionDeviceManager::class.java)
        val settings = getSharedPreferences("companion", MODE_PRIVATE)
        try {
            if (address in manager.associations) {
                CompanionLink.enable(this, address); toast("Systémové připojování zapnuto."); return
            }
            settings.edit().putString("pending", address).apply()
            // A fixed-address Bluetooth filter also finds an already bonded/connected lamp.
            val request = AssociationRequest.Builder().setSingleDevice(true)
                .addDeviceFilter(BluetoothDeviceFilter.Builder().setAddress(address).build()).build()
            manager.associate(request, object : CompanionDeviceManager.Callback() {
                override fun onDeviceFound(chooser: IntentSender) {
                    if (!alive || settings.getString("pending", "") != address) return
                    try { startIntentSenderForResult(chooser, 9, null, 0, 0, 0) }
                    catch (e: Exception) { settings.edit().remove("pending").apply(); error(e) }
                }
                override fun onFailure(message: CharSequence?) {
                    settings.edit().remove("pending").apply()
                    toast(message?.toString() ?: "Android nemohl schválit propojení.")
                }
            }, handler)
        } catch (e: Exception) { settings.edit().remove("pending").apply(); error(e) }
    }
    private fun showSettings() {
        val c = card(); eyebrow(c, "PŘIPOJENÍ A ZAŘÍZENÍ"); heading(c, "Tvoje sestava.")
        c.addView(label(if (demo != null) "UKÁZKA · změny neovládají skutečnou lampu." else "${client.savedName} · ${if (client.ready) "Bluetooth připojeno" else "Odpojeno"}", 15f, muted))
        c.addView(row(button("Připojení", "bluetooth") { showConnection() }, button("Odpojit") { ArrivalService.stop(this); CompanionLink.disable(this); demo = null; client.disconnect(); showConnection() }))
        c.addView(button("Systémové připojování", "bluetooth") { companionSettings() })
        val arrivalCard = card(); eyebrow(arrivalCard, "SVĚTLO TĚ PŘIVÍTÁ"); heading(arrivalCard, "Stačí přijít.")
        arrivalCard.background = artwork(mint, Color.rgb(101,226,224))
        arrivalCard.addView(label(if (app.arrivalPrefs.getBoolean("enabled", false)) app.arrivalStatus else "Jedno rozsvícení při příchodu. Potom je ovládání zase na tobě.", 15f, muted))
        arrivalCard.addView(button("Automatický příchod", "bluetooth") { arrivalSettings() })
        val quick = card(); eyebrow(quick, "PO RUCE I NA ZAMČENÉM TELEFONU"); heading(quick, "Jedním klepnutím.")
        quick.addView(label("Zapnout nebo zhasnout přímo z horní lišty telefonu.", 15f, muted))
        quick.addView(button("Světla v horní liště", "bulb") { tileSettings() })
        val lamps = card(); heading(lamps, "Světla a ovladač")
        lamps.addView(button("Přidat žárovku", "plus", true) {
            val content = column()
            content.addView(label("Zapni žárovku v párovacím režimu. Otevření Zigbee sítě na tři minuty umožní přidat ji do této sestavy.", 16f, muted))
            lateinit var dialog: AlertDialog
            content.addView(button("Otevřít síť na 3 minuty", "plus", true) { call("pair", json("seconds" to 180)) { toast("Zigbee párování otevřeno na 3 minuty."); dialog.dismiss(); scheduleRefresh() } })
            content.addView(button("Zavřít párování") { call("pair", json("seconds" to 0)) { toast("Párování uzavřeno."); dialog.dismiss() } })
            dialog = form("Přidat Zigbee žárovku", content)
        })
        lamps.addView(button("Spravovat skupiny", "bulb") { groupList() })
        lamps.addView(button("Nastavení IR ovladače", "settings") { remoteEditor() })
        lamps.addView(label("Názvy a chování jednotlivých světel najdeš v jejich vlastním nastavení.", 13f, muted))
        val web = card(); heading(web, "Síť a web lampy")
        val address = if (network.optBoolean("apActive")) "192.168.4.1" else if (network.optBoolean("staConnected")) network.optString("staIp") else ""
        web.addView(label(if (address.isEmpty()) "Wi-Fi, displej a aktualizace jsou na webu lampy." else "http://$address", 15f, muted))
        web.addView(button("Otevřít web lampy", "web") {
            if (demo != null) toast("V ukázce není připojená skutečná lampa.")
            else if (address.isBlank()) openPairingWeb()
            else try { startActivity(Intent(Intent.ACTION_VIEW, pairingUri(address).buildUpon().fragment(null).build())) }
                catch (e: Exception) { error(e) }
        })
        web.addView(button("Načíst síťový stav") { readNetwork() })
        val automation = card(); heading(automation, "Budík a časovače")
        automation.addView(button("Ranní světlo s budíkem", "bulb") { alarmSettings() })
        automation.addView(button("Časovač vypnutí", "settings") { timerSettings() })
        val more = card(); heading(more, "Aplikace")
        more.addView(button("Pomoc s párováním", "bluetooth") { pairingHelp() })
        more.addView(button("Zapomenout výběr lampy v aplikaci") { confirm("Smazat uloženou adresu v aplikaci? Párování Bluetooth v Androidu i žárovky zůstanou zachované.") { ArrivalService.stop(this); CompanionLink.disable(this, forget = true); app.arrivalPrefs.edit().clear().apply(); demo = null; client.forgetSaved(); showConnection() } })
        more.addView(button("Ukázka bez lampy", "bulb") { startDemo() })
        val version = packageManager.getPackageInfo(packageName, 0).versionName
        more.addView(label("Z-lamp $version · bez účtu, reklam a cloudu", 13f, muted))
        more.addView(label("Oblíbené barvy se ukládají jen v tomto telefonu. Nastavení světel a scén zůstává v lampě.", 13f, muted))
        more.addView(button("Technické informace") { form("O aplikaci", column().apply {
            addView(label("Firmware: ${client.info.optString("version", "nezjištěný")}\n\nAplikace používá Bluetooth LE. Na Androidu 8–11 potřebuje hledání oprávnění polohy; souřadnice nečte ani neukládá.\n\nPo výpadku se nedokončené změny automaticky neopakují. Znovu se připoj a ověř aktuální stav.\n\nAktualizace obou desek probíhají přes Wi-Fi ve webu lampy. Displej se BLE příkazem neprobouzí.", 15f, muted))
        }) })
    }
}

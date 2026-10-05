package dev.hadal

import android.app.Activity
import android.app.Application
import android.app.KeyguardManager
import android.app.PendingIntent
import android.appwidget.AppWidgetManager
import android.appwidget.AppWidgetProvider
import android.content.BroadcastReceiver
import android.content.ClipData
import android.content.ClipboardManager
import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.content.res.Configuration
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.hardware.biometrics.BiometricManager
import android.hardware.biometrics.BiometricPrompt
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.CancellationSignal
import android.os.Handler
import android.os.Looper
import android.provider.MediaStore
import android.provider.OpenableColumns
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.service.quicksettings.Tile
import android.service.quicksettings.TileService
import android.util.Base64
import android.view.HapticFeedbackConstants
import android.view.View
import android.widget.RemoteViews
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.positionChange
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.repeatOnLifecycle
import com.google.mlkit.vision.barcode.common.Barcode
import com.google.mlkit.vision.codescanner.GmsBarcodeScannerOptions
import com.google.mlkit.vision.codescanner.GmsBarcodeScanning
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.IOException
import java.net.HttpURLConnection
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.Socket
import java.net.URI
import java.net.URL
import java.net.URLEncoder
import java.security.KeyStore
import java.util.concurrent.LinkedBlockingQueue
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import kotlin.concurrent.thread
import kotlin.math.roundToInt

class ApiError(msg: String, val code: Int = 0) : Exception(msg)

// Pairing + connections
object Pc {
    private const val KEY = "hadal-token"
    const val UNREACHABLE = -1
    const val UNREACHABLE_MSG = "Can't reach the PC yet. Tailscale may still be waking up"
    fun prefs(ctx: Context) = ctx.getSharedPreferences("hadal", Context.MODE_PRIVATE)

    private fun key(): SecretKey {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (ks.getKey(KEY, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").apply {
            init(KeyGenParameterSpec.Builder(KEY, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .build())
        }.generateKey()
    }

    // Paired PCs

    data class Paired(val id: String, val addr: String, val host: String?)

    private fun load(ctx: Context) = JSONArray(prefs(ctx).getString("pcs", null) ?: "[]")

    private fun save(ctx: Context, a: JSONArray, cur: String?) = prefs(ctx).edit().putString("pcs", a.toString()).putString("cur", cur).apply()

    private fun cur(ctx: Context): JSONObject? {
        val a = load(ctx)
        val id = prefs(ctx).getString("cur", null)
        return (0 until a.length()).map(a::getJSONObject).firstOrNull { it.getString("id") == id } ?: if (a.length() > 0) a.getJSONObject(0) else null
    }

    private fun update(ctx: Context, change: (JSONObject) -> Unit) {
        val a = load(ctx)
        val id = cur(ctx)?.getString("id") ?: return
        for (i in 0 until a.length()) if (a.getJSONObject(i).getString("id") == id) change(a.getJSONObject(i))
        save(ctx, a, id)
    }

    fun all(ctx: Context): List<Paired> = load(ctx).let { a -> (0 until a.length()).map { a.getJSONObject(it).run { Paired(getString("id"), getString("addr"), optString("host").ifEmpty { null }) } } }
    fun current(ctx: Context): Paired? = cur(ctx)?.run { Paired(getString("id"), getString("addr"), optString("host").ifEmpty { null }) }
    fun select(ctx: Context, id: String) = prefs(ctx).edit().putString("cur", id).apply()
    fun paired(ctx: Context) = cur(ctx) != null

    fun unpair(ctx: Context) {
        val a = load(ctx)
        val id = cur(ctx)?.getString("id")
        val keep = JSONArray()
        for (i in 0 until a.length()) if (a.getJSONObject(i).getString("id") != id) keep.put(a.getJSONObject(i))
        save(ctx, keep, if (keep.length() > 0) keep.getJSONObject(0).getString("id") else null)
    }

    // QR: hadal://ip:port/token
    fun pair(ctx: Context, qr: String): Boolean {
        val m = Regex("""hadal://(100\.\d{1,3}\.\d{1,3}\.\d{1,3}):(\d{1,5})/([0-9a-f]{64})""").matchEntire(qr) ?: return false
        val c = Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.ENCRYPT_MODE, key()) }
        val tok = Base64.encodeToString(c.iv + c.doFinal(m.groupValues[3].toByteArray()), Base64.NO_WRAP)
        val addr = "${m.groupValues[1]}:${m.groupValues[2]}"
        val a = load(ctx)
        val same = (0 until a.length()).map(a::getJSONObject).firstOrNull { it.getString("addr") == addr }
        val id = same?.getString("id") ?: System.currentTimeMillis().toString(36)
        if (same != null) same.put("tok", tok) else a.put(JSONObject().put("id", id).put("addr", addr).put("tok", tok))
        save(ctx, a, id)
        return true
    }

    private fun token(ctx: Context): String? {
        val b = Base64.decode(cur(ctx)?.getString("tok") ?: return null, Base64.NO_WRAP)
        val c = Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, b, 0, 12)) }
        return String(c.doFinal(b, 12, b.size - 12))
    }

    private fun isTailnet(a: InetAddress) =
        a is Inet4Address && a.address[0] == 100.toByte() && (a.address[1].toInt() and 0xC0) == 0x40

    private fun tailscaleUp() = NetworkInterface.getNetworkInterfaces()?.toList().orEmpty().any { ni ->
        ni.isUp && ni.inetAddresses.toList().any(::isTailnet)
    }

    fun rememberHost(ctx: Context, host: String) {
        if (host.matches(Regex("[A-Za-z0-9-]{1,63}")) && current(ctx)?.host != host) update(ctx) { it.put("host", host) }
    }

    // MagicDNS fallback
    private fun relocate(ctx: Context, old: String): String? {
        val host = current(ctx)?.host ?: return null
        val ip = runCatching { InetAddress.getAllByName(host) }.getOrNull()?.firstOrNull(::isTailnet)?.hostAddress ?: return null
        val new = "$ip:${old.substringAfter(':')}"
        if (new == old) return null
        update(ctx) { it.put("addr", new) }
        return new
    }

    private fun <T> withTarget(ctx: Context, f: (addr: String, tok: String) -> T): T {
        val addr = current(ctx)?.addr
        val tok = runCatching { token(ctx) }.getOrNull()
        if (addr == null || tok == null) throw ApiError("Not paired: scan the QR code from the PC's tray menu")
        if (!tailscaleUp()) throw ApiError("Tailscale is off on this phone")
        return try {
            f(addr, tok)
        } catch (e: ApiError) {
            if (e.code != UNREACHABLE) throw e
            f(relocate(ctx, addr) ?: throw e, tok)
        }
    }

    private fun apiError(code: Int, body: ByteArray) = ApiError(
        if (code == 401) "Bad token: re-pair with the PC"
        else runCatching { JSONObject(String(body)).getString("error").replaceFirstChar { it.uppercase() } }.getOrDefault("HTTP $code"), code)

    fun call(ctx: Context, method: String, path: String, body: ByteArray? = null): ByteArray = withTarget(ctx) { addr, tok ->
        val c = URL("http://$addr$path").openConnection() as HttpURLConnection
        try {
            c.requestMethod = method
            c.connectTimeout = 4000
            c.readTimeout = 25000
            c.setRequestProperty("Authorization", "Bearer $tok")
            if (body != null) c.setRequestProperty("Content-Type", "text/plain; charset=utf-8")
            if (method == "POST") { c.doOutput = true; c.setFixedLengthStreamingMode(body?.size ?: 0) }
            try { c.connect() } catch (e: IOException) { throw ApiError(UNREACHABLE_MSG, UNREACHABLE) }
            if (method == "POST") c.outputStream.use { it.write(body ?: ByteArray(0)) }
            val code = c.responseCode
            val resp = (if (code < 400) c.inputStream else c.errorStream)?.use { it.readBytes() } ?: ByteArray(0)
            if (code != 200) throw apiError(code, resp)
            resp
        } catch (e: IOException) {
            throw ApiError("Connection to the PC failed")
        } finally {
            c.disconnect()
        }
    }

    fun fileName(raw: String?): String {
        var n = (raw ?: "").map { if (it < ' ' || it in "\\/:*?\"<>|") '_' else it }.joinToString("").trim(' ', '.')
        if (n.isEmpty()) n = "file"
        if (Regex("(?i)(con|prn|aux|nul|com\\d|lpt\\d)( *\\..*)?").matches(n)) n = "_$n"
        while (n.toByteArray().size > 200) n = n.dropLast(1)
        return n
    }

    private fun path(name: String) = URLEncoder.encode(name, "UTF-8").replace("+", "%20")

    fun upload(ctx: Context, uri: Uri, progress: (Float) -> Unit): String {
        var name: String? = null
        var size = -1L
        ctx.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)?.use { c ->
            if (c.moveToFirst()) { name = c.getString(0); if (!c.isNull(1)) size = c.getLong(1) }
        }
        if (size < 0) throw ApiError("Couldn't read that file's size")
        return withTarget(ctx) { addr, tok ->
            val c = URL("http://$addr/upload/${path(fileName(name))}").openConnection() as HttpURLConnection
            try {
                c.requestMethod = "POST"
                c.connectTimeout = 4000
                c.readTimeout = 60000
                c.setRequestProperty("Authorization", "Bearer $tok")
                c.doOutput = true
                c.setFixedLengthStreamingMode(size)
                try { c.connect() } catch (e: IOException) { throw ApiError(UNREACHABLE_MSG, UNREACHABLE) }
                val ins = ctx.contentResolver.openInputStream(uri) ?: throw ApiError("Couldn't open that file")
                ins.use { i ->
                    c.outputStream.use { o ->
                        val b = ByteArray(1 shl 16)
                        var sent = 0L
                        while (true) {
                            val n = i.read(b)
                            if (n < 0) break
                            o.write(b, 0, n)
                            sent += n
                            progress(if (size > 0) sent.toFloat() / size else 1f)
                        }
                    }
                }
                val code = c.responseCode
                val resp = (if (code < 400) c.inputStream else c.errorStream)?.use { it.readBytes() } ?: ByteArray(0)
                if (code != 200) throw apiError(code, resp)
                JSONObject(String(resp)).optString("msg")
            } catch (e: IOException) {
                throw ApiError("File transfer to the PC failed")
            } finally {
                c.disconnect()
            }
        }
    }

    fun download(ctx: Context, name: String, progress: (Float) -> Unit): String = withTarget(ctx) { addr, tok ->
        val c = URL("http://$addr/files/${path(name)}").openConnection() as HttpURLConnection
        val res = ctx.contentResolver
        var item: Uri? = null
        try {
            c.connectTimeout = 4000
            c.readTimeout = 60000
            c.setRequestProperty("Authorization", "Bearer $tok")
            try { c.connect() } catch (e: IOException) { throw ApiError(UNREACHABLE_MSG, UNREACHABLE) }
            val code = c.responseCode
            if (code != 200) throw apiError(code, c.errorStream?.use { it.readBytes() } ?: ByteArray(0))
            val size = c.contentLengthLong
            item = res.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, ContentValues().apply {
                put(MediaStore.Downloads.DISPLAY_NAME, name)
                put(MediaStore.Downloads.RELATIVE_PATH, "Download/Hadal")
                put(MediaStore.Downloads.IS_PENDING, 1)
            }) ?: throw ApiError("Couldn't create the file in Downloads")
            var got = 0L
            (res.openOutputStream(item) ?: throw ApiError("Couldn't write to Downloads")).use { o ->
                c.inputStream.use { i ->
                    val b = ByteArray(1 shl 16)
                    while (true) {
                        val n = i.read(b)
                        if (n < 0) break
                        o.write(b, 0, n)
                        got += n
                        if (size > 0) progress(got.toFloat() / size)
                    }
                }
            }
            if (size >= 0 && got != size) throw ApiError("Download interrupted")
            res.update(item, ContentValues().apply { put(MediaStore.Downloads.IS_PENDING, 0) }, null, null)
            item = null
            "Saved to Downloads/Hadal"
        } catch (e: IOException) {
            throw ApiError("File transfer from the PC failed")
        } finally {
            item?.let { runCatching { res.delete(it, null, null) } }
            c.disconnect()
        }
    }

    fun openStream(ctx: Context, path: String, readTimeoutMs: Int): Socket = withTarget(ctx) { addr, tok ->
        val s = Socket()
        try {
            s.connect(InetSocketAddress(addr.substringBefore(':'), addr.substringAfter(':').toInt()), 4000)
        } catch (e: IOException) {
            s.close()
            throw ApiError(UNREACHABLE_MSG, UNREACHABLE)
        }
        try {
            s.tcpNoDelay = true
            s.soTimeout = 25000
            s.getOutputStream().write("POST $path HTTP/1.1\r\nHost: $addr\r\nAuthorization: Bearer $tok\r\n\r\n".toByteArray())
            val ins = s.getInputStream()
            val head = StringBuilder()
            while (!head.endsWith("\r\n\r\n") && head.length < 8192) {
                val ch = ins.read()
                if (ch < 0) break
                head.append(ch.toChar())
            }
            val code = head.split(' ').getOrNull(1)?.toIntOrNull() ?: 0
            if (code != 200) throw apiError(code, ins.readBytes())
            s.soTimeout = readTimeoutMs
            s
        } catch (e: Exception) {
            s.close()
            throw e as? ApiError ?: ApiError("Connection to the PC failed")
        }
    }
}

// Touchpad
class Pad {
    private val q = LinkedBlockingQueue<String>()
    private var mx = 0f
    private var my = 0f
    private var sy = 0f
    private var ax = -1
    private var ay = -1
    private var armed = false

    @Synchronized fun move(x: Float, y: Float) { mx += x; my += y; wake() }
    @Synchronized fun point(x: Int, y: Int) { ax = x.coerceIn(0, 65535); ay = y.coerceIn(0, 65535); wake() }
    @Synchronized fun scroll(y: Float) { sy += y; wake() }
    fun send(line: String) { q.offer(line) }
    private fun wake() { if (!armed) { armed = true; q.offer("") } }

    @Synchronized private fun drain(): String {
        armed = false
        val sb = StringBuilder()
        if (ax >= 0) { sb.append("a $ax $ay\n"); ax = -1 }
        val ix = mx.toInt().coerceIn(-2000, 2000)
        val iy = my.toInt().coerceIn(-2000, 2000)
        if (ix != 0 || iy != 0) { sb.append("m $ix $iy\n"); mx -= ix; my -= iy }
        val s = sy.toInt().coerceIn(-500, 500)
        if (s != 0) { sb.append("s $s\n"); sy -= s }
        return sb.toString()
    }

    fun run(ctx: Context, onConnected: () -> Unit) {
        q.clear()
        Pc.openStream(ctx, "/input", 0).use { s ->
            onConnected()
            val out = s.getOutputStream()
            while (true) {
                val line = q.take()
                out.write((drain() + if (line.isEmpty()) "" else "$line\n").toByteArray())
                out.flush()
            }
        }
    }
}

fun haptic(view: View) {
    if (Pc.prefs(view.context).getBoolean("haptics", true)) view.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
}

// App lock
object AppLock {
    var unlocked by mutableStateOf(false)
    private var asked = false
    private var started = 0
    private var watching = false

    fun enabled(ctx: Context) = Pc.prefs(ctx).getBoolean("app_lock", false)
    fun locked(ctx: Context) = enabled(ctx) && !unlocked
    fun available(ctx: Context) = ctx.getSystemService(KeyguardManager::class.java).isDeviceSecure

    fun watch(app: Application) {
        if (watching) return
        watching = true
        app.registerActivityLifecycleCallbacks(object : Application.ActivityLifecycleCallbacks {
            override fun onActivityStarted(a: Activity) { started++ }
            override fun onActivityStopped(a: Activity) {
                if (--started == 0 && !a.isChangingConfigurations) { unlocked = false; asked = false }
            }
            override fun onActivityCreated(a: Activity, b: Bundle?) {}
            override fun onActivityResumed(a: Activity) {}
            override fun onActivityPaused(a: Activity) {}
            override fun onActivitySaveInstanceState(a: Activity, b: Bundle) {}
            override fun onActivityDestroyed(a: Activity) {}
        })
    }

    @Suppress("DEPRECATION")
    fun prompt(act: Activity, force: Boolean = false, onCancel: () -> Unit = {}) {
        if (!locked(act) || (asked && !force)) return
        asked = true
        val b = BiometricPrompt.Builder(act).setTitle("Unlock Hadal").setSubtitle("Controls your PC")
        if (Build.VERSION.SDK_INT >= 30) b.setAllowedAuthenticators(BiometricManager.Authenticators.BIOMETRIC_STRONG or BiometricManager.Authenticators.DEVICE_CREDENTIAL)
        else b.setDeviceCredentialAllowed(true)
        b.build().authenticate(CancellationSignal(), act.mainExecutor, object : BiometricPrompt.AuthenticationCallback() {
            override fun onAuthenticationSucceeded(result: BiometricPrompt.AuthenticationResult) { unlocked = true }
            override fun onAuthenticationError(code: Int, msg: CharSequence) { onCancel() }
        })
    }
}

@Composable
fun LockScreen(unlock: () -> Unit) = Abyss {
    Column(
        Modifier.fillMaxSize().pointerInput(Unit) { awaitEachGesture { awaitFirstDown().consume() } }.padding(32.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp, Alignment.CenterVertically),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Box(contentAlignment = Alignment.Center) {
            Box(Modifier.size(240.dp).background(Brush.radialGradient(listOf(Color(0x40FF3B30), Color.Transparent)), CircleShape))
            Emblem(Modifier.size(168.dp))
        }
        Text("HADAL", style = MaterialTheme.typography.headlineMedium, color = MaterialTheme.colorScheme.primary)
        Text("Locked", style = Plate.copy(fontSize = 14.sp, letterSpacing = 1.sp), color = MaterialTheme.colorScheme.secondary)
        Spacer(Modifier.height(8.dp))
        Button(unlock) { Text("Unlock") }
    }
}

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        AppLock.watch(application)
        setContent {
            HadalTheme {
                // Keep App composed under the lock
                Box {
                    App()
                    if (AppLock.locked(this@MainActivity)) LockScreen { AppLock.prompt(this@MainActivity, force = true) }
                }
            }
        }
    }

    override fun onResume() {
        super.onResume()
        if (Build.VERSION.SDK_INT >= 33) setRecentsScreenshotEnabled(!AppLock.enabled(this))
        AppLock.prompt(this)
    }
}

// Share target
class ShareActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        @Suppress("DEPRECATION")
        val files: List<Uri> = when (intent?.action) {
            Intent.ACTION_SEND_MULTIPLE -> intent.getParcelableArrayListExtra<Uri>(Intent.EXTRA_STREAM).orEmpty()
            else -> listOfNotNull(intent?.getParcelableExtra<Uri>(Intent.EXTRA_STREAM))
        }
        val text = intent?.getStringExtra(Intent.EXTRA_TEXT).orEmpty()
        val url = if (files.isNotEmpty()) null else Regex("""https?://\S+""").find(text)?.value?.trimEnd('.', ',', ';', ':', '!', '?', '"', '\'')
            ?.let { runCatching { URI(it).toASCIIString() }.getOrDefault(it) }
        AppLock.watch(application)
        AppLock.prompt(this, force = true, onCancel = ::finish)
        setContent {
            if (!AppLock.locked(this)) MaterialTheme(HadalColors, typography = HadalType) {
                val scope = rememberCoroutineScope()
                var sending by remember { mutableStateOf(false) }
                var progress by remember { mutableStateOf<String?>(null) }
                fun done(msg: String) { Toast.makeText(this@ShareActivity, msg, Toast.LENGTH_LONG).show(); finish() }
                AlertDialog(
                    onDismissRequest = { if (!sending) finish() },
                    title = { Text(if (files.isNotEmpty()) "Send to PC?" else "Open on PC?") },
                    text = {
                        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            Text(
                                when {
                                    files.size == 1 -> "1 file goes to Downloads\\Hadal on ${Pc.current(this@ShareActivity)?.host ?: "the PC"}."
                                    files.size > 1 -> "${files.size} files go to Downloads\\Hadal on ${Pc.current(this@ShareActivity)?.host ?: "the PC"}."
                                    else -> url ?: "There's nothing to send: no file and no web link."
                                },
                                maxLines = 6, overflow = TextOverflow.Ellipsis,
                            )
                            progress?.let { Text(it, style = Gauge.copy(fontSize = 12.sp), color = MaterialTheme.colorScheme.secondary) }
                        }
                    },
                    confirmButton = {
                        if (files.isNotEmpty() || url != null) TextButton({
                            sending = true
                            scope.launch {
                                if (url != null) {
                                    done(try { withContext(Dispatchers.IO) { Pc.call(this@ShareActivity, "POST", "/open", url.toByteArray()) }; "Opened on PC" }
                                         catch (e: Exception) { e.message ?: "Error" })
                                    return@launch
                                }
                                var last = ""
                                for ((i, f) in files.withIndex()) {
                                    try {
                                        last = withContext(Dispatchers.IO) {
                                            Pc.upload(this@ShareActivity, f) { p -> progress = "File ${i + 1} of ${files.size}: ${(p * 100).roundToInt()}%" }
                                        }
                                    } catch (e: Exception) { done(e.message ?: "Error"); return@launch }
                                }
                                done(if (files.size == 1) last else "Sent ${files.size} files to Downloads\\Hadal")
                            }
                        }, enabled = !sending) { Text(if (files.isNotEmpty()) "Send" else "Open") }
                    },
                    dismissButton = { if (!sending) TextButton(::finish) { Text("Cancel") } },
                )
            }
        }
    }
}

@Composable
fun App() {
    val ctx = LocalContext.current
    val view = LocalView.current
    val scope = rememberCoroutineScope()
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    val snack = remember { SnackbarHostState() }
    val prefs = remember { Pc.prefs(ctx) }
    val clipboard = remember { ctx.getSystemService(ClipboardManager::class.java) }
    var paired by remember { mutableStateOf(Pc.paired(ctx)) }
    var pairing by remember { mutableIntStateOf(0) }
    var tab by rememberSaveable { mutableIntStateOf(0) }
    var status by remember { mutableStateOf<JSONObject?>(null) }
    var error by remember { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(false) }
    var shot by remember { mutableStateOf<Bitmap?>(null) }
    var viewer by remember { mutableStateOf(false) }
    var timerDialog by remember { mutableStateOf(false) }
    var confirm by remember { mutableStateOf<Pair<String, String>?>(null) }

    fun say(msg: String) = scope.launch { snack.currentSnackbarData?.dismiss(); snack.showSnackbar(msg) }

    suspend fun refresh(): Boolean {
        try {
            val s = JSONObject(String(withContext(Dispatchers.IO) { Pc.call(ctx, "GET", "/status") }))
            Pc.rememberHost(ctx, s.optString("host"))
            status = s
            error = null
            Widget.show(ctx, s)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            status = null
            error = e.message ?: "Error"
            return (e as? ApiError)?.code != 401
        }
        return true
    }

    LaunchedEffect(paired, pairing) {
        if (!paired) return@LaunchedEffect
        // STARTED so it connects behind the prompt
        lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) { while (refresh()) delay(if (status == null) 1500 else 5000) }
    }

    fun scan() {
        GmsBarcodeScanning.getClient(ctx, GmsBarcodeScannerOptions.Builder().setBarcodeFormats(Barcode.FORMAT_QR_CODE).build())
            .startScan()
            .addOnSuccessListener { b ->
                if (Pc.pair(ctx, b.rawValue ?: "")) { paired = true; pairing++; tab = 0; say("Paired") }
                else say("That isn't a Hadal pairing QR code")
            }
            .addOnFailureListener { say("Scan failed: ${it.message}") }
    }

    fun act(
        path: String, method: String = "POST", body: ByteArray? = null,
        onDone: suspend (ByteArray) -> Unit = { say(JSONObject(String(it)).optString("msg")) },
    ) = scope.launch {
        haptic(view)
        busy = true
        try {
            onDone(withContext(Dispatchers.IO) { Pc.call(ctx, method, "/$path", body) })
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            say(e.message ?: "Error")
        }
        busy = false
        refresh()
    }

    fun powerAction(label: String, path: String) {
        if (prefs.getBoolean("confirm_$path", true)) confirm = label to path else act(path)
    }

    fun switchPc(id: String) { Pc.select(ctx, id); status = null; error = null; pairing++ }

    var transfer by remember { mutableStateOf<Pair<String, Float>?>(null) }
    var filesDialog by remember { mutableStateOf(false) }
    var audioDialog by remember { mutableStateOf(false) }
    fun upload(uris: List<Uri>) = scope.launch {
        for ((i, uri) in uris.withIndex()) {
            val label = if (uris.size > 1) "Sending ${i + 1} of ${uris.size}" else "Sending"
            transfer = label to 0f
            try {
                val msg = withContext(Dispatchers.IO) { Pc.upload(ctx, uri) { transfer = label to it } }
                say(msg)
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                say(e.message ?: "Upload failed"); break
            }
        }
        transfer = null
    }
    val pickFiles = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { if (it.isNotEmpty()) upload(it) }
    fun download(name: String) = scope.launch {
        transfer = "Receiving" to 0f
        try { say(withContext(Dispatchers.IO) { Pc.download(ctx, name) { transfer = "Receiving" to it } }) }
        catch (e: CancellationException) { throw e }
        catch (e: Exception) { say(e.message ?: "Download failed") }
        transfer = null
    }

    val landscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
    val tabs = listOf(R.drawable.ic_monitor to "Control", R.drawable.ic_chart to "Performance", R.drawable.ic_mouse to "Touchpad", R.drawable.ic_settings to "Settings")
    val pcs = remember(paired, pairing) { Pc.all(ctx) }

    Scaffold(
        containerColor = Color.Transparent,
        contentColor = MaterialTheme.colorScheme.onBackground,
        snackbarHost = { SnackbarHost(snack) },
        topBar = { if (paired) Header(tabs[tab].second, status, error, pcs, Pc.current(ctx)?.id, ::switchPc, ::scan) },
        bottomBar = {
            if (paired && !landscape) NavigationBar(containerColor = MaterialTheme.colorScheme.surfaceContainerLow.copy(alpha = 0.92f)) {
                tabs.forEachIndexed { i, (icon, label) ->
                    NavigationBarItem(tab == i, { tab = i }, { Icon(painterResource(icon), null) }, label = { Text(label, maxLines = 1) },
                        colors = NavigationBarItemDefaults.colors(
                            selectedIconColor = MaterialTheme.colorScheme.primary, selectedTextColor = MaterialTheme.colorScheme.primary,
                            indicatorColor = MaterialTheme.colorScheme.primaryContainer))
                }
            }
        },
    ) { pad ->
        Row(Modifier.padding(pad).imePadding()) {
            if (paired && landscape) NavigationRail(containerColor = MaterialTheme.colorScheme.surfaceContainerLow.copy(alpha = 0.92f)) {
                tabs.forEachIndexed { i, (icon, label) ->
                    NavigationRailItem(tab == i, { tab = i }, { Icon(painterResource(icon), null) }, label = { Text(label, maxLines = 1) },
                        colors = NavigationRailItemDefaults.colors(
                            selectedIconColor = MaterialTheme.colorScheme.primary, selectedTextColor = MaterialTheme.colorScheme.primary,
                            indicatorColor = MaterialTheme.colorScheme.primaryContainer))
                }
            }
            Box(Modifier.weight(1f)) {
                when {
                    !paired -> PairScreen(::scan)
                    tab == 1 -> PerfScreen()
                    tab == 2 -> TouchpadScreen()
                    tab == 3 -> SettingsScreen(status, ::scan) {
                        Pc.unpair(ctx)
                        paired = Pc.paired(ctx)
                        status = null
                        pairing++
                        tab = 0
                    }
                    else -> {
                        val session = status?.optJSONObject("session")
                        val first: @Composable ColumnScope.() -> Unit = {
                            StatusCard(status, error, onRetry = { pairing++ }, onCancelTimer = { act("timer/cancel") })
                            MediaCard(session, !busy, onSound = { audioDialog = true }) { act(it) }
                            Section("Quick") {
                                ActionRow {
                                    Action(R.drawable.ic_lock, "Lock", !busy) { act("lock") }
                                    Action(R.drawable.ic_monitor, "Screen off", !busy) { act("monitors_off") }
                                    Action(R.drawable.ic_screenshot, "Screenshot", !busy) {
                                        act("screenshot") { b ->
                                            shot = withContext(Dispatchers.Default) { BitmapFactory.decodeByteArray(b, 0, b.size) }
                                            if (shot == null) say("Couldn't decode the screenshot") else viewer = true
                                        }
                                    }
                                    Action(R.drawable.ic_live, "Stream", !busy) { haptic(view); ctx.startActivity(Intent(ctx, StreamActivity::class.java)) }
                                }
                            }
                            AppsSection(status != null, pairing) { act("app/$it") }
                        }
                        val second: @Composable ColumnScope.() -> Unit = {
                            Section("Files") {
                                ActionRow {
                                    Action(R.drawable.ic_upload, "Send to PC", transfer == null) { pickFiles.launch(arrayOf("*/*")) }
                                    Action(R.drawable.ic_download, "From PC", transfer == null) { filesDialog = true }
                                }
                                transfer?.let { (label, f) ->
                                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                                        Text(label, style = MaterialTheme.typography.labelMedium)
                                        Bar(f, Modifier.weight(1f))
                                        Text("${(f * 100).roundToInt()}%", style = Gauge.copy(fontSize = 12.sp))
                                    }
                                }
                            }
                            Section("Display") {
                                DisplayPicker(session?.optString("display"), !busy && session != null) { act("display/$it") }
                            }
                            Section("Clipboard") {
                                ActionRow {
                                    Action(R.drawable.ic_paste, "Send to PC", !busy) {
                                        val t = clipboard.primaryClip?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.coerceToText(ctx)?.toString()
                                        if (t.isNullOrEmpty()) say("Phone clipboard is empty") else act("clipboard", body = t.toByteArray())
                                    }
                                    Action(R.drawable.ic_copy, "Get from PC", !busy) {
                                        act("clipboard", "GET") { b ->
                                            val t = String(b)
                                            if (t.isEmpty()) say("PC clipboard has no text")
                                            else { clipboard.setPrimaryClip(ClipData.newPlainText("PC clipboard", t)); say("Copied ${t.length} characters from the PC") }
                                        }
                                    }
                                }
                            }
                            Section("Power") {
                                ActionRow {
                                    Action(R.drawable.ic_sleep, "Sleep", !busy) { powerAction("Sleep", "sleep") }
                                    Action(R.drawable.ic_restart, "Restart", !busy) { powerAction("Restart", "restart") }
                                    Action(R.drawable.ic_power, "Shut down", !busy) { powerAction("Shut down", "shutdown") }
                                    Action(R.drawable.ic_timer, "Timer", !busy) { timerDialog = true }
                                }
                            }
                        }
                        if (landscape) Row(
                            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
                            horizontalArrangement = Arrangement.spacedBy(16.dp),
                        ) {
                            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(16.dp), content = first)
                            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(16.dp), content = second)
                        } else Column(
                            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
                            verticalArrangement = Arrangement.spacedBy(16.dp),
                        ) { first(); second() }
                    }
                }
            }
        }
    }

    if (filesDialog) FilesDialog({ filesDialog = false }) { filesDialog = false; download(it) }
    if (audioDialog) AudioDialog { audioDialog = false }

    confirm?.let { (label, path) ->
        AlertDialog(
            onDismissRequest = { confirm = null },
            title = { Text("$label the PC?") },
            confirmButton = { TextButton({ confirm = null; act(path) }) { Text(label) } },
            dismissButton = { TextButton({ confirm = null }) { Text("Cancel") } },
        )
    }

    if (timerDialog) {
        var what by remember { mutableStateOf("shutdown") }
        var mins by remember { mutableIntStateOf(30) }
        AlertDialog(
            onDismissRequest = { timerDialog = false },
            title = { Text("Power timer") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
                    val acts = listOf("sleep" to "Sleep", "restart" to "Restart", "shutdown" to "Shut down")
                    SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth()) {
                        acts.forEachIndexed { i, (k, l) ->
                            SegmentedButton(what == k, { what = k }, SegmentedButtonDefaults.itemShape(i, acts.size), icon = {}, colors = hadalSegments()) { Text(l, maxLines = 1) }
                        }
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        listOf(15, 30, 60, 120).forEach { m ->
                            FilterChip(mins == m, { mins = m }, label = { Text(if (m < 60) "$m min" else "${m / 60} h") })
                        }
                    }
                }
            },
            confirmButton = { TextButton({ timerDialog = false; act("timer/$what/$mins") }) { Text("Start") } },
            dismissButton = { TextButton({ timerDialog = false }) { Text("Cancel") } },
        )
    }

    if (viewer) {
        Dialog({ viewer = false }, DialogProperties(usePlatformDefaultWidth = false)) {
            MaterialTheme(darkColorScheme()) {
                var scale by remember { mutableFloatStateOf(1f) }
                var offset by remember { mutableStateOf(Offset.Zero) }
                Box(Modifier.fillMaxSize().background(Color.Black).pointerInput(Unit) {
                    detectTransformGestures { _, pan, zoom, _ -> scale = (scale * zoom).coerceIn(1f, 8f); offset += pan }
                }) {
                    val bmp = shot
                    if (bmp != null) Image(
                        bmp.asImageBitmap(), "PC screen",
                        Modifier.fillMaxSize().graphicsLayer(scaleX = scale, scaleY = scale, translationX = offset.x, translationY = offset.y),
                        contentScale = ContentScale.Fit,
                    ) else CircularProgressIndicator(Modifier.align(Alignment.Center))
                    TextButton({ viewer = false }, Modifier.align(Alignment.TopEnd).safeDrawingPadding().padding(8.dp)) { Text("Close") }
                }
            }
        }
    }
}

@Composable
fun PairScreen(scan: () -> Unit) = Column(
    Modifier.fillMaxSize().padding(32.dp),
    verticalArrangement = Arrangement.spacedBy(16.dp, Alignment.CenterVertically),
    horizontalAlignment = Alignment.CenterHorizontally,
) {
    Box(contentAlignment = Alignment.Center) {
        Box(Modifier.size(240.dp).background(Brush.radialGradient(listOf(Color(0x40FF3B30), Color.Transparent)), CircleShape))
        Emblem(Modifier.size(168.dp))
    }
    Text("HADAL", style = MaterialTheme.typography.headlineMedium, color = MaterialTheme.colorScheme.primary)
    Text("Your PC, from the deep.", style = Plate.copy(fontSize = 14.sp, letterSpacing = 1.sp), color = MaterialTheme.colorScheme.secondary)
    Spacer(Modifier.height(8.dp))
    Text("On the PC, right-click the helmet in the tray → Show pairing QR, then scan it here.",
        textAlign = TextAlign.Center, color = MaterialTheme.colorScheme.onSurfaceVariant)
    Button(scan, contentPadding = PaddingValues(horizontal = 28.dp, vertical = 14.dp)) { Text("Scan pairing QR") }
}

@Composable
fun Emblem(modifier: Modifier) = Box(modifier.clip(CircleShape).border(1.dp, MaterialTheme.colorScheme.primary.copy(alpha = 0.5f), CircleShape)) {
    Image(painterResource(R.mipmap.ic_launcher_background), null, Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
    Image(painterResource(R.mipmap.ic_launcher_foreground), null, Modifier.fillMaxSize().graphicsLayer(scaleX = 1.3f, scaleY = 1.3f))
}

@Composable
fun Header(title: String, status: JSONObject?, error: String?, pcs: List<Pc.Paired>, current: String?, onPick: (String) -> Unit, onAdd: () -> Unit) = Row(
    Modifier.fillMaxWidth().statusBarsPadding().padding(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 6.dp),
    verticalAlignment = Alignment.CenterVertically,
) {
    Emblem(Modifier.size(34.dp))
    Spacer(Modifier.width(12.dp))
    Text(title.uppercase(), Modifier.weight(1f), style = Plate.copy(fontSize = if (title.length > 9) 16.sp else 19.sp, letterSpacing = 1.sp),
        color = MaterialTheme.colorScheme.primary, maxLines = 1, overflow = TextOverflow.Ellipsis)
    val waking = error == Pc.UNREACHABLE_MSG
    val dot = when {
        error != null && !waking -> MaterialTheme.colorScheme.tertiary
        status != null -> MaterialTheme.colorScheme.secondary
        else -> MaterialTheme.colorScheme.outline
    }
    var menu by remember { mutableStateOf(false) }
    Box {
        Row(
            Modifier.clip(CircleShape).background(MaterialTheme.colorScheme.surfaceContainerHigh.copy(alpha = 0.85f))
                .border(1.dp, MaterialTheme.colorScheme.outlineVariant, CircleShape).clickable { menu = true }
                .padding(horizontal = 12.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(Modifier.size(14.dp), contentAlignment = Alignment.Center) {
                Box(Modifier.size(14.dp).background(Brush.radialGradient(listOf(dot.copy(alpha = 0.55f), Color.Transparent)), CircleShape))
                Box(Modifier.size(7.dp).background(dot, CircleShape))
            }
            Spacer(Modifier.width(6.dp))
            Text(status?.optString("host") ?: if (error != null && !waking) "Offline" else "Connecting…", Modifier.widthIn(max = 132.dp),
                style = MaterialTheme.typography.labelMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(" ▾", style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        DropdownMenu(menu, { menu = false }) {
            pcs.forEach { p ->
                DropdownMenuItem(
                    { Text(p.host ?: p.addr.substringBefore(':')) },
                    { menu = false; if (p.id != current) onPick(p.id) },
                    leadingIcon = { RadioButton(p.id == current, null) },
                )
            }
            HorizontalDivider()
            DropdownMenuItem({ Text("Pair another PC") }, { menu = false; onAdd() })
        }
    }
}

@Composable
fun Section(title: String, content: @Composable ColumnScope.() -> Unit) = Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
    Text(title.uppercase(), style = Plate.copy(fontSize = 13.sp), color = MaterialTheme.colorScheme.primary)
    content()
}

@Composable
fun ActionRow(content: @Composable RowScope.() -> Unit) =
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp), content = content)

@Composable
fun RowScope.Action(icon: Int, label: String, enabled: Boolean, onClick: () -> Unit) = FilledTonalButton(
    onClick, Modifier.weight(1f).height(76.dp), enabled = enabled, shape = RoundedCornerShape(20.dp), contentPadding = PaddingValues(4.dp),
) {
    Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Icon(painterResource(icon), null, tint = MaterialTheme.colorScheme.primary)
        Text(label, style = MaterialTheme.typography.labelMedium, maxLines = 1)
    }
}

@Composable
fun DisplayPicker(current: String?, enabled: Boolean, onPick: (String) -> Unit) {
    val modes = listOf("internal" to "PC only", "clone" to "Duplicate", "extend" to "Extend", "external" to "2nd only")
    SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth()) {
        modes.forEachIndexed { i, (m, label) ->
            SegmentedButton(
                current == m, { onPick(m) }, SegmentedButtonDefaults.itemShape(i, modes.size), enabled = enabled, icon = {},
                colors = hadalSegments(),
            ) {
                Text(label, maxLines = 1, style = MaterialTheme.typography.labelMedium)
            }
        }
    }
}

fun uptime(s: Long) = when {
    s >= 86400 -> "${s / 86400}d ${s % 86400 / 3600}h"
    s >= 3600 -> "${s / 3600}h ${s % 3600 / 60}m"
    else -> "${s / 60}m"
}

fun remaining(s: Long) = when {
    s < 60 -> "under a minute"
    s < 3600 -> "${(s + 59) / 60} min"
    else -> "${s / 3600} h ${(s % 3600 + 59) / 60} min"
}

fun gb(mb: Long) = "%.1f".format(mb / 1024f)

@Composable
fun Meter(label: String, fraction: Float, text: String) = Row(verticalAlignment = Alignment.CenterVertically) {
    Text(label, Modifier.width(44.dp), style = MaterialTheme.typography.labelMedium)
    Bar(fraction, Modifier.weight(1f))
    Text(text, Modifier.width(112.dp), textAlign = TextAlign.End, style = Gauge.copy(fontSize = 12.sp))
}

@Composable
fun StatusCard(s: JSONObject?, error: String?, onRetry: () -> Unit, onCancelTimer: () -> Unit) = Panel(Modifier.fillMaxWidth()) {
    val red = MaterialTheme.colorScheme.error
    val dim = MaterialTheme.colorScheme.onSurfaceVariant
    Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        when {
            error == Pc.UNREACHABLE_MSG -> Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                Text(error, Modifier.weight(1f), color = dim)
            }
            error != null -> Row(verticalAlignment = Alignment.CenterVertically) {
                Text(error, Modifier.weight(1f), color = red, style = MaterialTheme.typography.titleMedium)
                TextButton(onRetry) { Text("Retry") }
            }
            s == null -> Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                Text("Connecting…")
            }
            else -> {
                val session = s.optJSONObject("session")
                val user = if (s.isNull("user")) null else s.getString("user")
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        when {
                            user == null -> "Session state unknown"
                            user.isEmpty() -> "No user logged in"
                            s.getBoolean("locked") -> "$user · locked"
                            else -> "$user · unlocked"
                        },
                        Modifier.weight(1f), style = MaterialTheme.typography.titleMedium, maxLines = 1, overflow = TextOverflow.Ellipsis,
                    )
                    Text("up ${uptime(s.getLong("uptime"))}", style = Gauge.copy(fontSize = 12.sp), color = dim)
                }
                session?.optString("fg")?.takeIf { it.isNotEmpty() }?.let {
                    Text("In front: $it", color = dim, style = MaterialTheme.typography.bodyMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
                val cpu = s.getInt("cpu")
                val used = s.getLong("ramUsedMb")
                val total = s.getLong("ramTotalMb").coerceAtLeast(1)
                Meter("CPU", cpu / 100f, "$cpu%")
                Meter("RAM", used.toFloat() / total, "${gb(used)} / ${gb(total)} GB")
                s.optJSONObject("timer")?.let { t ->
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(painterResource(R.drawable.ic_timer), null, Modifier.size(18.dp), tint = MaterialTheme.colorScheme.primary)
                        Spacer(Modifier.width(8.dp))
                        val act = t.optString("action").replaceFirstChar { it.uppercase() }
                        Text("$act in ${remaining(t.optLong("remaining"))}", Modifier.weight(1f))
                        TextButton(onCancelTimer) { Text("Cancel") }
                    }
                }
                if (!user.isNullOrEmpty() && !s.getBoolean("tray")) Text("Tray app not running on the PC", color = red)
                if (s.getBoolean("paused")) Text("Remote control is paused on the PC", color = red)
            }
        }
    }
}

// AUMID to app name
fun appName(aumid: String): String {
    val n = aumid.substringBefore('!').substringBefore('_').removeSuffix(".exe").substringAfterLast('.').replaceFirstChar { it.uppercase() }
    return if (n.length > 24 || n.matches(Regex("[0-9A-Fa-f]{8,}|\\d+"))) "" else n
}

@Composable
fun MediaCard(session: JSONObject?, enabled: Boolean, onSound: () -> Unit, act: (String) -> Unit) = Panel(Modifier.fillMaxWidth()) {
    val media = session?.optJSONObject("media")
    val dim = MaterialTheme.colorScheme.onSurfaceVariant
    val playing = media?.optBoolean("playing") == true
    Column(Modifier.padding(horizontal = 16.dp, vertical = 12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                if (media != null) {
                    Text(media.optString("title").ifEmpty { "Unknown title" }, style = MaterialTheme.typography.titleMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    val sub = listOf(media.optString("artist"), appName(media.optString("app"))).filter { it.isNotEmpty() }.joinToString(" · ")
                    if (sub.isNotEmpty()) Text(sub, color = dim, style = MaterialTheme.typography.bodyMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                } else Text("Nothing playing", style = MaterialTheme.typography.titleMedium, color = dim)
            }
            if (media != null) Text(
                if (playing) "Playing" else "Paused",
                Modifier.padding(start = 8.dp).clip(CircleShape)
                    .background(if (playing) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceContainerHighest)
                    .padding(horizontal = 10.dp, vertical = 4.dp),
                color = if (playing) MaterialTheme.colorScheme.onPrimaryContainer else dim,
                style = MaterialTheme.typography.labelMedium,
            )
        }
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp, Alignment.CenterHorizontally), verticalAlignment = Alignment.CenterVertically) {
            FilledTonalIconButton({ act("prev") }, enabled = enabled) { Icon(painterResource(R.drawable.ic_prev), "Previous") }
            FilledIconButton({ act("playpause") }, Modifier.size(48.dp), enabled = enabled) {
                Icon(painterResource(if (playing) R.drawable.ic_pause else R.drawable.ic_play), if (playing) "Pause" else "Play")
            }
            FilledTonalIconButton({ act("next") }, enabled = enabled) { Icon(painterResource(R.drawable.ic_next), "Next") }
        }
        val vol = session?.optInt("vol", -1) ?: -1
        val muted = session?.optBoolean("muted") == true
        var drag by remember { mutableStateOf<Float?>(null) }
        LaunchedEffect(vol) { drag = null }
        Row(verticalAlignment = Alignment.CenterVertically) {
            IconButton({ act("mute") }, enabled = enabled && vol >= 0) {
                Icon(painterResource(if (muted) R.drawable.ic_mute else R.drawable.ic_vol_up), if (muted) "Unmute" else "Mute")
            }
            HadalSlider(
                value = drag ?: (vol.coerceAtLeast(0) / 100f),
                onValueChange = { drag = it },
                onValueChangeFinished = { drag?.let { act("volume/${(it * 100).roundToInt()}") } },
                modifier = Modifier.weight(1f),
                enabled = enabled && vol >= 0,
            )
            Text(if (vol < 0) "-" else "${((drag ?: (vol / 100f)) * 100).roundToInt()}%", Modifier.width(48.dp), textAlign = TextAlign.End)
            IconButton(onSound, enabled = enabled) { Icon(painterResource(R.drawable.ic_mixer), "Sound output and app volumes") }
        }
    }
}

@Composable
fun rememberGet(path: String, key: Any? = Unit): Pair<JSONObject?, String?> {
    val ctx = LocalContext.current
    var data by remember(key) { mutableStateOf<JSONObject?>(null) }
    var err by remember(key) { mutableStateOf<String?>(null) }
    LaunchedEffect(key) {
        try { data = JSONObject("{\"v\":" + String(withContext(Dispatchers.IO) { Pc.call(ctx, "GET", path) }) + "}") }
        catch (e: CancellationException) { throw e }
        catch (e: Exception) { err = e.message ?: "Error" }
    }
    return data to err
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun AppsSection(online: Boolean, key: Int, start: (Int) -> Unit) {
    if (!online) return
    val (data, _) = rememberGet("/apps", key)
    val apps = data?.optJSONArray("v") ?: return
    Section("Apps") {
        if (apps.length() == 0) Text("Add apps on the PC: tray menu → Apps the phone can start.",
            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            for (i in 0 until apps.length()) {
                val a = apps.getJSONObject(i)
                FilledTonalButton({ start(a.getInt("id")) }) { Text(a.getString("name"), maxLines = 1) }
            }
        }
    }
}

@Composable
fun FilesDialog(onDismiss: () -> Unit, onPick: (String) -> Unit) {
    val (data, err) = rememberGet("/files")
    val files = data?.optJSONArray("v")
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Files on the PC") },
        text = {
            Column(Modifier.heightIn(max = 420.dp).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                when {
                    err != null -> Text(err, color = MaterialTheme.colorScheme.error)
                    files == null -> CircularProgressIndicator(Modifier.size(24.dp))
                    files.length() == 0 -> Text("Nothing to download. On the PC, put files in Downloads\\Hadal\\To phone, or use the tray menu → Send a file to the phone.")
                    else -> for (i in 0 until files.length()) {
                        val f = files.getJSONObject(i)
                        Row(Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).clickable { onPick(f.getString("name")) }.padding(vertical = 10.dp, horizontal = 4.dp),
                            verticalAlignment = Alignment.CenterVertically) {
                            Icon(painterResource(R.drawable.ic_download), null, tint = MaterialTheme.colorScheme.primary)
                            Spacer(Modifier.width(12.dp))
                            Text(f.getString("name"), Modifier.weight(1f), maxLines = 2, overflow = TextOverflow.Ellipsis)
                            Text(bytes(f.getLong("size")), style = Gauge.copy(fontSize = 12.sp), color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                }
            }
        },
        confirmButton = { TextButton(onDismiss) { Text("Close") } },
    )
}

fun bytes(b: Long) = when {
    b >= 1L shl 30 -> "%.1f GB".format(b / 1073741824f)
    b >= 1L shl 20 -> "%.1f MB".format(b / 1048576f)
    else -> "${(b + 1023) / 1024} KB"
}

@Composable
fun AudioDialog(onDismiss: () -> Unit) {
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    var reload by remember { mutableIntStateOf(0) }
    var note by remember { mutableStateOf<String?>(null) }
    val (data, err) = rememberGet("/audio", reload)
    val v = data?.optJSONObject("v")
    fun post(path: String) = scope.launch {
        note = try { JSONObject(String(withContext(Dispatchers.IO) { Pc.call(ctx, "POST", path) })).optString("msg") } catch (e: Exception) { e.message }
        reload++
    }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Sound") },
        text = {
            Column(Modifier.heightIn(max = 480.dp).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                note?.let { Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant) }
                when {
                    err != null -> Text(err, color = MaterialTheme.colorScheme.error)
                    v == null -> CircularProgressIndicator(Modifier.size(24.dp))
                    else -> {
                        Text("OUTPUT", style = Plate.copy(fontSize = 13.sp), color = MaterialTheme.colorScheme.primary)
                        val outs = v.optJSONArray("outputs") ?: JSONArray()
                        for (i in 0 until outs.length()) {
                            val o = outs.getJSONObject(i)
                            Row(Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).clickable { if (!o.getBoolean("default")) post("/audio/out/${o.getString("id")}") },
                                verticalAlignment = Alignment.CenterVertically) {
                                RadioButton(o.getBoolean("default"), null)
                                Text(o.getString("name"), maxLines = 2, overflow = TextOverflow.Ellipsis)
                            }
                        }
                        Spacer(Modifier.height(4.dp))
                        Text("APPS", style = Plate.copy(fontSize = 13.sp), color = MaterialTheme.colorScheme.primary)
                        val apps = v.optJSONArray("apps") ?: JSONArray()
                        if (apps.length() == 0) Text("No app is playing sound.", color = MaterialTheme.colorScheme.onSurfaceVariant)
                        for (i in 0 until apps.length()) {
                            val a = apps.getJSONObject(i)
                            val pid = a.getLong("pid")
                            var drag by remember(pid, a.getInt("vol")) { mutableStateOf<Float?>(null) }
                            Text(a.getString("name"), style = MaterialTheme.typography.labelLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                IconButton({ post("/audio/app/$pid/mute") }) {
                                    Icon(painterResource(if (a.getBoolean("muted")) R.drawable.ic_mute else R.drawable.ic_vol_up), if (a.getBoolean("muted")) "Unmute" else "Mute")
                                }
                                HadalSlider(drag ?: (a.getInt("vol") / 100f), { drag = it }, Modifier.weight(1f),
                                    onValueChangeFinished = { drag?.let { post("/audio/app/$pid/${(it * 100).roundToInt()}") } })
                                Text("${((drag ?: (a.getInt("vol") / 100f)) * 100).roundToInt()}%", Modifier.width(44.dp), textAlign = TextAlign.End)
                            }
                        }
                    }
                }
            }
        },
        confirmButton = { TextButton(onDismiss) { Text("Close") } },
    )
}

// Performance

@Composable
fun Sparkline(values: List<Float>, description: String) {
    val line = MaterialTheme.colorScheme.secondary
    val base = MaterialTheme.colorScheme.outlineVariant
    Canvas(Modifier.fillMaxWidth().height(56.dp).semantics { contentDescription = description }) {
        drawLine(base, Offset(0f, size.height), Offset(size.width, size.height), 1.dp.toPx())
        if (values.size < 2) return@Canvas
        val step = size.width / (HISTORY - 1)
        val x0 = size.width - (values.size - 1) * step
        val path = Path()
        values.forEachIndexed { i, v ->
            val x = x0 + i * step
            val y = size.height * (1 - v.coerceIn(0f, 100f) / 100f)
            if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
        }
        val area = Path().apply { addPath(path); lineTo(size.width, size.height); lineTo(x0, size.height); close() }
        drawPath(area, line.copy(alpha = 0.12f))
        drawPath(path, line, style = Stroke(2.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round))
    }
}

const val HISTORY = 60

fun rate(bps: Long) = when {
    bps >= 1 shl 20 -> "%.1f MB/s".format(bps / 1048576f)
    bps >= 1024 -> "%.0f KB/s".format(bps / 1024f)
    else -> "$bps B/s"
}

@Composable
fun PerfCard(title: String, value: String, subtitle: String? = null, content: @Composable ColumnScope.() -> Unit) =
    Panel(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = Alignment.Bottom) {
                Column(Modifier.weight(1f)) {
                    Text(title, style = MaterialTheme.typography.titleMedium)
                    subtitle?.let { Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1, overflow = TextOverflow.Ellipsis) }
                }
                Text(value, style = Gauge.copy(fontSize = 26.sp), color = MaterialTheme.colorScheme.primary)
            }
            content()
        }
    }

fun JSONArray?.floats() = if (this == null) emptyList() else List(length()) { optInt(it).toFloat() }

@Composable
fun RowScope.SummaryTile(label: String, value: String) = Column(
    Modifier.weight(1f).clip(RoundedCornerShape(16.dp)).background(MaterialTheme.colorScheme.secondaryContainer).padding(vertical = 10.dp),
    horizontalAlignment = Alignment.CenterHorizontally,
) {
    Text(value, style = Gauge.copy(fontSize = 22.sp), color = MaterialTheme.colorScheme.primary)
    Text(label, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSecondaryContainer)
}

@Composable
fun PerfScreen() {
    val ctx = LocalContext.current
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    val dim = MaterialTheme.colorScheme.onSurfaceVariant
    var perf by remember { mutableStateOf<JSONObject?>(null) }
    var err by remember { mutableStateOf<String?>(null) }

    LaunchedEffect(Unit) {
        lifecycle.repeatOnLifecycle(Lifecycle.State.RESUMED) {
            while (true) {
                try {
                    perf = JSONObject(String(withContext(Dispatchers.IO) { Pc.call(ctx, "GET", "/perf") }))
                    err = null
                } catch (e: CancellationException) {
                    throw e
                } catch (e: Exception) {
                    err = e.message ?: "Error"
                    if ((e as? ApiError)?.code == 401) return@repeatOnLifecycle
                }
                delay(2000)
            }
        }
    }

    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        err?.let { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.titleMedium) }
        val p = perf
        val cpu = p?.optJSONObject("cpu")
        if (p == null || cpu == null) {
            if (err == null) Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                Text("Collecting…")
            }
            return@Column
        }
        val gpus = p.optJSONArray("gpus")
        val ram = p.optJSONObject("ram")
        val ramPct = ram?.let { it.optLong("usedMb") * 100 / it.optLong("totalMb").coerceAtLeast(1) }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            SummaryTile("CPU", "${cpu.optInt("total")}%")
            if (gpus != null && gpus.length() > 0) SummaryTile("GPU", "${gpus.getJSONObject(0).optInt("util")}%")
            if (ramPct != null) SummaryTile("RAM", "$ramPct%")
        }
        PerfCard("CPU", "${cpu.optInt("total")}%", cpu.optString("name")) {
            Sparkline(cpu.optJSONArray("hist").floats(), "CPU usage, last 2 minutes")
            val cores = cpu.optJSONArray("cores")
            if (cores != null && cores.length() > 1) {
                Text("${cores.length()} logical processors", style = MaterialTheme.typography.labelMedium, color = dim)
                val bar = MaterialTheme.colorScheme.secondary
                val track = MaterialTheme.colorScheme.surfaceContainerHighest
                Row(Modifier.fillMaxWidth().height(40.dp), horizontalArrangement = Arrangement.spacedBy(2.dp)) {
                    for (i in 0 until cores.length()) {
                        val v = cores.optInt(i) / 100f
                        Box(Modifier.weight(1f).fillMaxHeight().clip(RoundedCornerShape(2.dp)).background(track), contentAlignment = Alignment.BottomCenter) {
                            Box(Modifier.fillMaxWidth().fillMaxHeight(v.coerceIn(0.02f, 1f)).background(bar))
                        }
                    }
                }
            }
        }
        ram?.let { r ->
            val used = r.optLong("usedMb")
            val total = r.optLong("totalMb").coerceAtLeast(1)
            PerfCard("Memory", "$ramPct%", "${gb(used)} of ${gb(total)} GB in use") {
                Bar(used.toFloat() / total, Modifier.fillMaxWidth())
            }
        }
        if (gpus != null) for (i in 0 until gpus.length()) {
            val g = gpus.getJSONObject(i)
            val vu = g.optLong("vramUsedMb")
            val vt = g.optLong("vramTotalMb")
            PerfCard(if (gpus.length() > 1) "GPU ${i + 1}" else "GPU", "${g.optInt("util")}%", g.optString("name")) {
                Sparkline(g.optJSONArray("hist").floats(), "GPU ${i + 1} usage, last 2 minutes")
                if (vt >= 512) Meter("VRAM", vu.toFloat() / vt, "${gb(vu)} / ${gb(vt)} GB")
                if (g.has("tempC")) Meter("Temp", g.optInt("tempC") / 100f, "${g.optInt("tempC")} °C")
                if (g.has("fanPct")) Meter("Fan", g.optInt("fanPct") / 100f, "${g.optInt("fanPct")}%")
                if (g.has("powerW")) Text("Drawing ${g.optInt("powerW")} W", style = MaterialTheme.typography.labelMedium, color = dim)
            }
        }
        p.optJSONObject("battery")?.let { b ->
            val pct = b.optInt("pct")
            val secs = b.optLong("secsLeft", -1)
            val state = when {
                b.optBoolean("charging") -> "Charging"
                b.optBoolean("plugged") -> "Plugged in"
                secs > 0 -> "${remaining(secs)} left"
                else -> "On battery"
            }
            PerfCard("Battery", "$pct%", state) { Bar(pct / 100f, Modifier.fillMaxWidth(), if (pct <= 15 && !b.optBoolean("plugged")) MaterialTheme.colorScheme.tertiary else MaterialTheme.colorScheme.primary) }
        }
        val devices = p.optJSONArray("devices")
        if (devices != null && devices.length() > 0) Panel(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text("Devices", style = MaterialTheme.typography.titleMedium)
                for (i in 0 until devices.length()) {
                    val d = devices.getJSONObject(i)
                    val name = d.optString("name").removeSuffix(" Hands-Free AG").removeSuffix(" Hands-Free").removeSuffix(" Stereo")
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(name, Modifier.weight(1f), style = MaterialTheme.typography.labelLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Text("${d.optInt("pct")}%", style = Gauge.copy(fontSize = 12.sp))
                    }
                    Bar(d.optInt("pct") / 100f, Modifier.fillMaxWidth())
                }
            }
        }
        val disks = p.optJSONArray("disks")
        if (disks != null && disks.length() > 0) Panel(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text("Disks", style = MaterialTheme.typography.titleMedium)
                for (i in 0 until disks.length()) {
                    val d = disks.getJSONObject(i)
                    val used = d.optLong("usedGb")
                    val total = d.optLong("totalGb").coerceAtLeast(1)
                    val active = d.optInt("active", -1)
                    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        Row {
                            Text(d.optString("name"), Modifier.weight(1f), style = MaterialTheme.typography.labelLarge)
                            Text("${total - used} GB free of $total" + if (active >= 0) " · $active% busy" else "", style = MaterialTheme.typography.labelMedium, color = dim)
                        }
                        Bar(used.toFloat() / total, Modifier.fillMaxWidth())
                    }
                }
            }
        }
        p.optJSONObject("net")?.let { n ->
            Panel(Modifier.fillMaxWidth()) {
                Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text("Network", Modifier.weight(1f), style = MaterialTheme.typography.titleMedium)
                    Column(horizontalAlignment = Alignment.End) {
                        Text("↓ ${rate(n.optLong("rxBps"))}", style = MaterialTheme.typography.titleSmall)
                        Text("↑ ${rate(n.optLong("txBps"))}", style = MaterialTheme.typography.titleSmall, color = dim)
                    }
                }
            }
        }
        Text("GPU temperature needs an NVIDIA GPU. Windows doesn't expose CPU temperature without a driver.", style = MaterialTheme.typography.bodySmall, color = dim)
    }
}

// Touchpad

@Composable
fun TouchpadScreen() {
    val ctx = LocalContext.current
    val view = LocalView.current
    val pad = remember { Pad() }
    val speed = remember { Pc.prefs(ctx).getFloat("speed", 1.5f) }
    var conn by remember { mutableStateOf<String?>("Connecting…") }
    var attempt by remember { mutableIntStateOf(0) }
    var holding by remember { mutableStateOf(false) }

    val lifecycle = LocalLifecycleOwner.current.lifecycle
    DisposableEffect(attempt) {
        conn = "Connecting…"
        holding = false
        val t = keepConnected(pad, ctx, lifecycle, { conn = null }) { conn = it }
        onDispose { t.interrupt() }
    }

    Column(Modifier.fillMaxSize().padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        conn?.let { c ->
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(c, Modifier.weight(1f), color = if (c == "Connecting…") MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.error)
                if (c != "Connecting…") TextButton({ attempt++ }) { Text("Reconnect") }
            }
        }
        Box(
            Modifier.fillMaxWidth().weight(1f).clip(RoundedCornerShape(24.dp)).background(MaterialTheme.colorScheme.surfaceVariant)
                .pointerInput(speed) {
                    awaitEachGesture {
                        val start = awaitFirstDown().uptimeMillis
                        var fingers = 1
                        var travel = 0f
                        var end: Long
                        while (true) {
                            val ev = awaitPointerEvent()
                            val down = ev.changes.filter { it.pressed }
                            if (down.isEmpty()) { end = ev.changes.first().uptimeMillis; break }
                            fingers = maxOf(fingers, down.size)
                            val d = down.fold(Offset.Zero) { acc, c -> acc + c.positionChange() } / down.size.toFloat()
                            travel += d.getDistance()
                            if (fingers == 1) pad.move(d.x * speed, d.y * speed) else if (down.size >= 2) pad.scroll(d.y)
                            ev.changes.forEach { it.consume() }
                        }
                        if (travel < viewConfiguration.touchSlop && end - start < 300) {
                            pad.send(if (fingers >= 2) "c r" else "c l")
                            haptic(view)
                        }
                    }
                },
            contentAlignment = Alignment.Center,
        ) {
            Text(
                "Drag to move · tap to click\nTwo-finger tap: right-click · two-finger drag: scroll",
                textAlign = TextAlign.Center, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        ActionRow {
            FilledTonalButton({ pad.send("c l") }, Modifier.weight(1f)) { Text("Left") }
            val holdColors = if (holding) ButtonDefaults.buttonColors() else ButtonDefaults.filledTonalButtonColors()
            Button({ holding = !holding; pad.send(if (holding) "d l" else "u l") }, Modifier.weight(1f), colors = holdColors) {
                Text(if (holding) "Release" else "Hold")
            }
            FilledTonalButton({ pad.send("c r") }, Modifier.weight(1f)) { Text("Right") }
        }
        TypeBar(pad)
    }
}

// Auto reconnect
fun keepConnected(pad: Pad, ctx: Context, lifecycle: Lifecycle, onUp: () -> Unit, onDown: (String) -> Unit) = thread {
    try {
        while (true) {
            if (lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) {
                try {
                    pad.run(ctx, onUp)
                    onDown("Disconnected from the PC. Reconnecting…")
                } catch (e: InterruptedException) {
                    throw e
                } catch (e: Exception) {
                    onDown(((e as? ApiError)?.message ?: "Disconnected from the PC") + ". Retrying…")
                }
            }
            Thread.sleep(2000)
        }
    } catch (_: InterruptedException) {}
}

// Typing
@Composable
fun TypeBar(pad: Pad, modifier: Modifier = Modifier) = Column(modifier, verticalArrangement = Arrangement.spacedBy(8.dp)) {
    var text by remember { mutableStateOf("") }
    OutlinedTextField(
        text,
        { new ->
            val keep = text.commonPrefixWith(new).length
            repeat(text.length - keep) { pad.send("k backspace") }
            new.substring(keep).filter { it >= ' ' }.chunked(250).forEach { pad.send("t $it") }
            text = new
        },
        Modifier.fillMaxWidth(),
        label = { Text("Type on PC") },
        leadingIcon = { Icon(painterResource(R.drawable.ic_keyboard), null) },
        singleLine = true,
        keyboardOptions = KeyboardOptions(imeAction = ImeAction.Send),
        keyboardActions = KeyboardActions(onSend = { pad.send("k enter"); text = "" }),
    )
    Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        listOf("Esc" to "esc", "Tab" to "tab", "Win" to "win", "←" to "left", "↑" to "up", "↓" to "down", "→" to "right",
            "Del" to "delete", "⌫" to "backspace", "⏎" to "enter", "PgUp" to "pgup", "PgDn" to "pgdn", "F5" to "f5", "F11" to "f11").forEach { (label, k) ->
            FilledTonalButton({ pad.send("k $k") }, Modifier.height(40.dp), contentPadding = PaddingValues(horizontal = 14.dp)) { Text(label) }
        }
    }
    Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        listOf("Copy" to "ctrl+c", "Paste" to "ctrl+v", "Cut" to "ctrl+x", "Undo" to "ctrl+z", "Redo" to "ctrl+y", "All" to "ctrl+a",
            "Save" to "ctrl+s", "Alt+Tab" to "alt+tab", "Desktop" to "win+d", "Tasks" to "win+tab", "Snip" to "win+shift+s",
            "New tab" to "ctrl+t", "Close tab" to "ctrl+w", "Next tab" to "ctrl+tab", "Reopen tab" to "ctrl+shift+t",
            "Task Mgr" to "ctrl+shift+esc", "Close app" to "alt+f4").forEach { (label, k) ->
            OutlinedButton({ pad.send("k $k") }, Modifier.height(36.dp), contentPadding = PaddingValues(horizontal = 12.dp)) {
                Text(label, style = MaterialTheme.typography.labelMedium)
            }
        }
    }
}

// Settings

@Composable
fun SwitchRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) = Row(
    Modifier.fillMaxWidth().toggleable(checked, role = Role.Switch, onValueChange = onChange).padding(vertical = 4.dp),
    verticalAlignment = Alignment.CenterVertically,
) {
    Text(label, Modifier.weight(1f))
    Switch(checked, null)
}

@Composable
fun SettingsScreen(status: JSONObject?, scan: () -> Unit, unpair: () -> Unit) {
    val ctx = LocalContext.current
    val prefs = remember { Pc.prefs(ctx) }
    var haptics by remember { mutableStateOf(prefs.getBoolean("haptics", true)) }
    var speed by remember { mutableFloatStateOf(prefs.getFloat("speed", 1.5f)) }
    var askUnpair by remember { mutableStateOf(false) }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(20.dp)) {
        Section("Feedback") {
            SwitchRow("Vibrate on taps", haptics) { haptics = it; prefs.edit().putBoolean("haptics", it).apply() }
        }
        Section("Security") {
            var lock by remember { mutableStateOf(AppLock.enabled(ctx)) }
            SwitchRow("Require fingerprint to open", lock) {
                if (it && !AppLock.available(ctx)) {
                    Toast.makeText(ctx, "Set up a screen lock on this phone first", Toast.LENGTH_LONG).show()
                    return@SwitchRow
                }
                AppLock.unlocked = true
                lock = it
                prefs.edit().putBoolean("app_lock", it).apply()
            }
            Text("Face or fingerprint, or your phone PIN. The quick toggles and the widget still work without it.",
                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Section("Ask before") {
            for ((key, label) in listOf("sleep" to "Sleep", "restart" to "Restart", "shutdown" to "Shut down")) {
                var on by remember { mutableStateOf(prefs.getBoolean("confirm_$key", true)) }
                SwitchRow(label, on) { on = it; prefs.edit().putBoolean("confirm_$key", it).apply() }
            }
        }
        Section("Touchpad speed") {
            Row(verticalAlignment = Alignment.CenterVertically) {
                HadalSlider(speed, { speed = it; prefs.edit().putFloat("speed", it).apply() }, Modifier.weight(1f), valueRange = 0.5f..4f)
                Text("%.1f×".format(speed), Modifier.width(48.dp), textAlign = TextAlign.End)
            }
        }
        Section("Stream") {
            ChoiceRow("Resolution", "stream_h", 1080, listOf(720 to "720p", 1080 to "1080p", 1440 to "1440p"))
            ChoiceRow("Frame rate", "stream_fps", 60, listOf(30 to "30", 60 to "60", 120 to "120"))
            ChoiceRow("Bitrate", "stream_mbps", 12, listOf(3 to "3", 6 to "6", 12 to "12", 25 to "25", 50 to "50"), " Mbit/s")
            Text("Away from home, or if the picture lags, use 3-6 Mbit/s.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            var ll by remember { mutableStateOf(prefs.getBoolean("stream_ll", false)) }
            SwitchRow("Low latency", ll) { ll = it; prefs.edit().putBoolean("stream_ll", it).apply() }
            Text("Less delay for games and pointer work, slightly blurrier in fast motion.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Section(if (Pc.all(ctx).size > 1) "Selected PC" else "Paired PC") {
            val cur = Pc.current(ctx)
            val host = status?.optString("host") ?: cur?.host ?: "Unknown"
            Text("$host · ${cur?.addr ?: ""}")
            if (Pc.all(ctx).size > 1) Text("Switch PCs from the name at the top right.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            ActionRow {
                FilledTonalButton(scan, Modifier.weight(1f)) { Text("Re-pair") }
                OutlinedButton({ askUnpair = true }, Modifier.weight(1f)) { Text("Unpair") }
            }
        }
        Section("About") {
            val version = runCatching { ctx.packageManager.getPackageInfo(ctx.packageName, 0).versionName }.getOrNull() ?: "?"
            Text("Hadal $version", color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
    if (askUnpair) AlertDialog(
        onDismissRequest = { askUnpair = false },
        title = { Text("Unpair from this PC?") },
        text = { Text("The token is deleted from this phone. You'll need to scan the QR code again.") },
        confirmButton = { TextButton({ askUnpair = false; unpair() }) { Text("Unpair") } },
        dismissButton = { TextButton({ askUnpair = false }) { Text("Cancel") } },
    )
}

// Tiles

open class ActionTile(private val path: String) : TileService() {
    override fun onStartListening() {
        qsTile?.run { state = Tile.STATE_INACTIVE; subtitle = null; updateTile() }
    }

    override fun onClick() {
        val t = qsTile ?: return
        t.state = Tile.STATE_ACTIVE
        t.subtitle = "…"
        t.updateTile()
        thread {
            val r = try { JSONObject(String(Pc.call(this, "POST", "/$path"))).optString("msg") } catch (e: Exception) { e.message }
            runCatching { t.subtitle = r; t.state = Tile.STATE_INACTIVE; t.updateTile() }
        }
    }
}
class LockTile : ActionTile("lock")
class PlayPauseTile : ActionTile("playpause")
class MonitorsOffTile : ActionTile("monitors_off")

// Widget

class Widget : AppWidgetProvider() {
    companion object {
        fun show(ctx: Context, s: JSONObject?, text: String? = null) {
            val mgr = AppWidgetManager.getInstance(ctx)
            val ids = mgr.getAppWidgetIds(android.content.ComponentName(ctx, Widget::class.java))
            if (ids.isEmpty()) return
            val line = text ?: s?.let {
                val total = it.optLong("ramTotalMb").coerceAtLeast(1)
                "${it.optString("host")} · CPU ${it.optInt("cpu")}% · RAM ${it.optLong("ramUsedMb") * 100 / total}%"
            } ?: return
            val v = RemoteViews(ctx.packageName, R.layout.widget)
            v.setTextViewText(R.id.w_info, line)
            mgr.partiallyUpdateAppWidget(ids, v)
        }

        fun refresh(ctx: Context) = try { show(ctx, JSONObject(String(Pc.call(ctx, "GET", "/status")))) } catch (e: Exception) { show(ctx, null, e.message ?: "Offline") }
    }

    override fun onUpdate(ctx: Context, mgr: AppWidgetManager, ids: IntArray) {
        val v = RemoteViews(ctx.packageName, R.layout.widget)
        for ((id, path) in WidgetClick.buttons) {
            val i = Intent(ctx, WidgetClick::class.java).putExtra("path", path)
            v.setOnClickPendingIntent(id, PendingIntent.getBroadcast(ctx, id, i, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT))
        }
        mgr.updateAppWidget(ids, v)
        val done = goAsync()
        thread { refresh(ctx); done.finish() }
    }
}

class WidgetClick : BroadcastReceiver() {
    companion object {
        val buttons = mapOf(
            R.id.w_lock to "lock", R.id.w_monitors to "monitors_off", R.id.w_prev to "prev", R.id.w_play to "playpause",
            R.id.w_next to "next", R.id.w_voldown to "vol_down", R.id.w_volup to "vol_up", R.id.w_info to "refresh",
        )
    }

    override fun onReceive(ctx: Context, intent: Intent) {
        val path = intent.getStringExtra("path")?.takeIf { it in buttons.values } ?: return
        val done = goAsync()
        thread {
            if (path == "refresh") { Widget.show(ctx, null, "Refreshing…"); Widget.refresh(ctx); done.finish(); return@thread }
            val msg = try { JSONObject(String(Pc.call(ctx, "POST", "/$path"))).optString("msg") } catch (e: Exception) { e.message ?: "Error" }
            Handler(Looper.getMainLooper()).post { Toast.makeText(ctx, msg, Toast.LENGTH_SHORT).show(); done.finish() }
        }
    }
}

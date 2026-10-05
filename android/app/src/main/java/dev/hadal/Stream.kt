package dev.hadal

import android.content.Context
import android.graphics.SurfaceTexture
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.view.Surface
import android.view.TextureView
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculateCentroidSize
import androidx.compose.foundation.gestures.calculatePan
import androidx.compose.foundation.gestures.calculateZoom
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.gestures.waitForUpOrCancellation
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.translate
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.AwaitPointerEventScope
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.positionChange
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.compose.LocalLifecycleOwner
import kotlinx.coroutines.delay
import java.io.BufferedInputStream
import java.io.DataInputStream
import java.io.IOException
import java.net.Socket
import java.nio.ByteBuffer
import kotlin.concurrent.thread
import kotlin.math.abs
import kotlin.math.roundToInt

data class StreamInfo(val w: Int, val h: Int, val ox: Int, val oy: Int, val ow: Int, val oh: Int, val monitor: Int, val monitors: Int)

class StreamClient(private val ctx: Context, private val surface: Surface) {
    @Volatile var muted = false
    @Volatile var stopped = false
        private set
    @Volatile private var socket: Socket? = null

    fun stop() { stopped = true; runCatching { socket?.close() } }

    fun run(h: Int, fps: Int, mbps: Int, monitor: Int, lowLatency: Boolean, onInfo: (StreamInfo) -> Unit, onCursor: (Int, Int, Boolean) -> Unit,
            onLatency: (Int) -> Unit = {}) {
        val s = Pc.openStream(ctx, "/stream/$h/$fps/$mbps/$monitor/${if (lowLatency) 1 else 0}", 15000)
        socket = s
        if (stopped) { s.close(); return }
        var rttMs = -1.0
        var encMs = 0
        var decSum = 0.0
        var decN = 0
        val queued = HashMap<Long, Long>()
        val pinger = thread {
            try {
                val out = s.getOutputStream()
                while (!stopped) {
                    out.write(ByteBuffer.allocate(9).put('p'.code.toByte()).putLong(System.nanoTime()).array())
                    out.flush()
                    Thread.sleep(1000)
                }
            } catch (_: Exception) {}
        }
        var codec: MediaCodec? = null
        var track: AudioTrack? = null
        var info: StreamInfo? = null
        val bi = MediaCodec.BufferInfo()
        var pts = 0L
        try {
            val ins = DataInputStream(BufferedInputStream(s.getInputStream(), 1 shl 16))
            while (!stopped) {
                val type = ins.readUnsignedByte().toChar()
                val len = Integer.reverseBytes(ins.readInt())
                if (len < 0 || len > (16 shl 20)) throw IOException("bad packet")
                val p = ByteArray(len)
                ins.readFully(p)
                when (type) {
                    'H' -> ints(p, 9)?.let { v -> info = StreamInfo(v[0], v[1], v[3], v[4], v[5], v[6], v[7], v[8]).also(onInfo) }
                    'R' -> { track?.release(); track = String(p).toIntOrNull()?.let(::audioTrack) }
                    'A' -> if (!muted) track?.write(p, 0, p.size, AudioTrack.WRITE_NON_BLOCKING)
                    'C' -> ints(p, 3)?.let { onCursor(it[0], it[1], it[2] == 1) }
                    'E' -> throw ApiError(String(p))
                    'L' -> encMs = String(p).toIntOrNull() ?: encMs
                    'P' -> if (p.size == 8) {
                        val ms = (System.nanoTime() - ByteBuffer.wrap(p).long) / 1e6
                        rttMs = if (rttMs < 0) ms else rttMs * 0.7 + ms * 0.3
                        val dec = if (decN > 0) decSum / decN else 0.0
                        decSum = 0.0; decN = 0
                        onLatency((rttMs / 2 + encMs + dec).roundToInt())
                    }
                    'V' -> {
                        val i = info ?: continue
                        val c = codec ?: decoder(p, i)?.also { codec = it } ?: continue
                        val ix = c.dequeueInputBuffer(100_000)
                        if (ix >= 0) {
                            c.getInputBuffer(ix)!!.apply { clear(); put(p) }
                            c.queueInputBuffer(ix, 0, p.size, pts, 0)
                            queued[pts] = System.nanoTime()
                            if (queued.size > 120) queued.clear()
                            pts += 16_666
                        }
                        while (true) {
                            val o = c.dequeueOutputBuffer(bi, 0)
                            if (o >= 0) {
                                queued.remove(bi.presentationTimeUs)?.let { decSum += (System.nanoTime() - it) / 1e6; decN++ }
                                c.releaseOutputBuffer(o, true)
                            } else if (o == MediaCodec.INFO_TRY_AGAIN_LATER) break
                        }
                    }
                }
            }
        } catch (e: IOException) {
            if (!stopped) throw ApiError("Stream interrupted")
        } finally {
            pinger.interrupt()
            runCatching { codec?.stop() }
            runCatching { codec?.release() }
            track?.release()
            runCatching { s.close() }
        }
    }

    private fun ints(p: ByteArray, n: Int) = String(p).split(' ').mapNotNull { it.toIntOrNull() }.takeIf { it.size == n }

    private fun nals(b: ByteArray): List<ByteArray> {
        val starts = mutableListOf<Int>()
        var i = 0
        while (i + 3 <= b.size) {
            if (b[i].toInt() == 0 && b[i + 1].toInt() == 0 && b[i + 2].toInt() == 1) { starts += i + 3; i += 3 } else i++
        }
        return starts.mapIndexed { k, s ->
            var e = if (k + 1 < starts.size) starts[k + 1] - 3 else b.size
            if (k + 1 < starts.size && e > s && b[e - 1].toInt() == 0) e--
            b.copyOfRange(s, e)
        }
    }

    private fun decoder(au: ByteArray, i: StreamInfo): MediaCodec? {
        val units = nals(au)
        val sps = units.firstOrNull { it.isNotEmpty() && (it[0].toInt() and 0x1F) == 7 } ?: return null
        val pps = units.firstOrNull { it.isNotEmpty() && (it[0].toInt() and 0x1F) == 8 } ?: return null
        val sc = byteArrayOf(0, 0, 0, 1)
        fun format(lowLatency: Boolean) = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, i.w, i.h).apply {
            setByteBuffer("csd-0", ByteBuffer.wrap(sc + sps))
            setByteBuffer("csd-1", ByteBuffer.wrap(sc + pps))
            setInteger(MediaFormat.KEY_PRIORITY, 0)
            if (lowLatency && Build.VERSION.SDK_INT >= 30) setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
        }
        val c = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        try { c.configure(format(true), surface, null, 0) } catch (e: Exception) { c.reset(); c.configure(format(false), surface, null, 0) }
        c.start()
        return c
    }

    private fun audioTrack(rate: Int): AudioTrack {
        val min = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT)
        return AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_MOVIE).build())
            .setAudioFormat(AudioFormat.Builder().setSampleRate(rate).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
            .setBufferSizeInBytes(maxOf(min * 2, rate * 4 / 8))
            .setTransferMode(AudioTrack.MODE_STREAM)
            .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
            .build().apply { play() }
    }
}

class StreamActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        WindowCompat.getInsetsController(window, window.decorView).run {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
        setContent { MaterialTheme(HadalColors, typography = HadalType) { StreamScreen(onClose = ::finish) } }
    }

    override fun onStop() {
        super.onStop()
        finish()
    }
}

@Composable
fun StreamScreen(onClose: () -> Unit) { // Not "close", clashes with Path.close()
    val ctx = LocalContext.current
    val view = LocalView.current
    val prefs = remember { Pc.prefs(ctx) }
    val pad = remember { Pad() }
    var surface by remember { mutableStateOf<Surface?>(null) }
    var client by remember { mutableStateOf<StreamClient?>(null) }
    var info by remember { mutableStateOf<StreamInfo?>(null) }
    var cursor by remember { mutableStateOf(Triple(0, 0, false)) }
    var status by remember { mutableStateOf<String?>("Connecting…") }
    var attempt by remember { mutableIntStateOf(0) }
    var mode by remember { mutableIntStateOf(0) } // View, direct, trackpad
    val controls = mode != 0
    val trackpad = mode == 2
    val speed = remember { prefs.getFloat("speed", 1.5f) }
    var picSize by remember { mutableStateOf(IntSize.Zero) }
    var showPointer by remember { mutableStateOf(prefs.getBoolean("stream_pointer", true)) }
    var monitor by remember { mutableIntStateOf(prefs.getInt("stream_monitor", 0)) }
    var keyboard by remember { mutableStateOf(false) }
    var holding by remember { mutableStateOf(false) }
    var muted by remember { mutableStateOf(false) }
    var padError by remember { mutableStateOf<String?>(null) }
    var scale by remember { mutableFloatStateOf(1f) }
    var offset by remember { mutableStateOf(Offset.Zero) }
    var latency by remember { mutableIntStateOf(-1) }
    var sideOpen by remember { mutableStateOf(true) }
    var barShown by remember { mutableStateOf(true) }
    var barTick by remember { mutableIntStateOf(0) }
    fun poke() { barShown = true; barTick++ }
    LaunchedEffect(barShown, barTick, status) { if (barShown && status == null) { delay(3000); barShown = false } }

    DisposableEffect(surface, attempt, monitor) {
        val c = surface?.let { StreamClient(ctx, it) }
        client = c
        if (c != null) {
            status = "Connecting…"
            c.muted = muted
            thread {
                try {
                    c.run(prefs.getInt("stream_h", 1080), prefs.getInt("stream_fps", 60), prefs.getInt("stream_mbps", 12), monitor,
                        prefs.getBoolean("stream_ll", false), { info = it; status = null }, { x, y, v -> cursor = Triple(x, y, v) }, { latency = it })
                    if (!c.stopped) status = "Stream ended"
                } catch (e: Exception) {
                    if (!c.stopped) status = e.message ?: "Stream ended"
                }
            }
        }
        onDispose { c?.stop() }
    }

    val lifecycle = LocalLifecycleOwner.current.lifecycle
    DisposableEffect(controls) {
        if (!controls) return@DisposableEffect onDispose {}
        padError = null
        val t = keepConnected(pad, ctx, lifecycle, { padError = null }) { padError = it }
        onDispose { t.interrupt() }
    }

    // Follow pointer when zoomed
    LaunchedEffect(cursor, scale, trackpad, picSize) {
        if (!trackpad || scale <= 1f || picSize.width == 0) return@LaunchedEffect
        val w = picSize.width.toFloat()
        val h = picSize.height.toFloat()
        val px = cursor.first / 65535f * w
        val py = cursor.second / 65535f * h
        val mx = (scale - 1) * w / 2
        val my = (scale - 1) * h / 2
        offset = Offset((-(px - w / 2) * scale).coerceIn(-mx, mx), (-(py - h / 2) * scale).coerceIn(-my, my))
    }

    Box(
        Modifier.fillMaxSize().background(Color.Black).then(
            if (controls) Modifier else Modifier.pointerInput(Unit) {
                detectTransformGestures { _, pan, zoom, _ ->
                    scale = (scale * zoom).coerceIn(1f, 6f)
                    offset = if (scale == 1f) Offset.Zero else offset + pan
                }
            }.pointerInput(Unit) { detectTapGestures { if (barShown) barShown = false else poke() } },
        ),
        contentAlignment = Alignment.Center,
    ) {
        val i = info
        // Controls beside the picture
        Row(Modifier.fillMaxSize()) {
            Box(
                Modifier.weight(1f).fillMaxHeight().then(
                    if (trackpad) Modifier.trackpadInput(
                        speed,
                        move = { x, y -> pad.move(x, y) },
                        click = { right -> pad.send(if (right) "c r" else "c l"); haptic(view) },
                        scroll = { pad.scroll(it) },
                        zoom = { z -> scale = (scale * z).coerceIn(1f, 6f); if (scale == 1f) offset = Offset.Zero },
                    ) else Modifier,
                ),
                contentAlignment = if (controls) Alignment.CenterEnd else Alignment.Center,
            ) {
                Box(
                    Modifier.aspectRatio(i?.let { it.w.toFloat() / it.h } ?: (16f / 10f))
                        .onSizeChanged { picSize = it }
                        .graphicsLayer(scaleX = scale, scaleY = scale, translationX = offset.x, translationY = offset.y)
                        .then(
                            if (mode == 1 && i != null) Modifier.directInput(
                                i,
                                scale = { scale },
                                point = { p, sz ->
                                    pad.point(i.ox + ((p.x / sz.width).coerceIn(0f, 1f) * i.ow).toInt(), i.oy + ((p.y / sz.height).coerceIn(0f, 1f) * i.oh).toInt())
                                },
                                click = { right -> pad.send(if (right) "c r" else "c l"); haptic(view) },
                                scroll = { pad.scroll(it) },
                                zoom = { z, pan ->
                                    scale = (scale * z).coerceIn(1f, 6f)
                                    offset = if (scale == 1f) Offset.Zero else offset + pan * scale
                                },
                            ) else Modifier,
                        ),
                ) {
                    AndroidView({ c ->
                        TextureView(c).apply {
                            surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                                override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) { surface = Surface(st) }
                                override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) {}
                                override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean { surface?.release(); surface = null; return true }
                                override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}
                            }
                        }
                    }, Modifier.fillMaxSize())
                    Canvas(Modifier.fillMaxSize()) {
                        val (x, y, visible) = cursor
                        if (!showPointer || !visible || i == null) return@Canvas
                        val s = 1.dp.toPx() / scale
                        val arrow = Path().apply {
                            moveTo(0f, 0f); lineTo(0f, 17 * s); lineTo(4.5f * s, 13 * s); lineTo(7.5f * s, 20 * s)
                            lineTo(10 * s, 19 * s); lineTo(7 * s, 12 * s); lineTo(12.5f * s, 12 * s); close()
                        }
                        translate(x / 65535f * size.width, y / 65535f * size.height) {
                            drawPath(arrow, Color.White)
                            drawPath(arrow, Color.Black, style = Stroke(1.2f * s))
                        }
                    }
                }
            }
            if (controls && !sideOpen) Box(Modifier.fillMaxHeight().padding(end = 8.dp).width(40.dp), contentAlignment = Alignment.Center) {
                FilledTonalIconButton({ sideOpen = true }, Modifier.size(40.dp)) {
                    Text("‹", style = MaterialTheme.typography.titleLarge, color = MaterialTheme.colorScheme.primary)
                }
            }
            if (controls && sideOpen) Column(
                Modifier.fillMaxHeight().padding(start = 8.dp, end = 8.dp, top = 60.dp, bottom = 8.dp).width(64.dp),
                verticalArrangement = Arrangement.SpaceEvenly,
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                padError?.let { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.labelSmall) }
                FilledTonalButton({ pad.send("c l"); haptic(view) }, Modifier.fillMaxWidth().height(52.dp), contentPadding = PaddingValues(0.dp)) {
                    Icon(painterResource(R.drawable.ic_click_left), "Left click", Modifier.size(28.dp), tint = MaterialTheme.colorScheme.primary)
                }
                FilledTonalButton({ pad.send("c r"); haptic(view) }, Modifier.fillMaxWidth().height(52.dp), contentPadding = PaddingValues(0.dp)) {
                    Icon(painterResource(R.drawable.ic_click_right), "Right click", Modifier.size(28.dp), tint = MaterialTheme.colorScheme.primary)
                }
                val hold = if (holding) ButtonDefaults.buttonColors() else ButtonDefaults.filledTonalButtonColors()
                Button({ holding = !holding; pad.send(if (holding) "d l" else "u l") }, Modifier.fillMaxWidth().height(52.dp), colors = hold,
                    contentPadding = PaddingValues(0.dp)) { Text(if (holding) "Release" else "Hold", maxLines = 1, style = MaterialTheme.typography.labelMedium) }
                val kb = if (keyboard) ButtonDefaults.buttonColors() else ButtonDefaults.filledTonalButtonColors()
                Button({ keyboard = !keyboard }, Modifier.fillMaxWidth().height(52.dp), colors = kb, contentPadding = PaddingValues(0.dp)) {
                    Icon(painterResource(R.drawable.ic_keyboard), "Keyboard")
                }
                FilledTonalIconButton({ sideOpen = false; keyboard = false }, Modifier.size(40.dp)) {
                    Text("›", style = MaterialTheme.typography.titleLarge, color = MaterialTheme.colorScheme.primary)
                }
            }
        }

        status?.let { s ->
            Column(Modifier.align(Alignment.Center), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(12.dp)) {
                if (s == "Connecting…") CircularProgressIndicator()
                Text(s, color = Color.White)
                if (s != "Connecting…") FilledTonalButton({ attempt++ }) { Text("Reconnect") }
            }
        }

        AnimatedVisibility(barShown || status != null, Modifier.align(Alignment.TopEnd), enter = fadeIn(), exit = fadeOut()) {
            Row(
                Modifier.safeDrawingPadding().padding(8.dp).background(Color.Black.copy(alpha = 0.62f), RoundedCornerShape(28.dp))
                    .padding(horizontal = 8.dp, vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                TextButton(onClose, colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.tertiary)) { Text("End") }
                if (latency >= 0) Text("≈ $latency ms", style = Gauge.copy(fontSize = 12.sp), color = MaterialTheme.colorScheme.secondary)
                FilledTonalIconButton({ poke(); muted = !muted; client?.muted = muted }) {
                    Icon(painterResource(if (muted) R.drawable.ic_mute else R.drawable.ic_vol_up), if (muted) "Unmute" else "Mute")
                }
                FilterChip(showPointer, { poke(); showPointer = !showPointer; prefs.edit().putBoolean("stream_pointer", showPointer).apply() },
                    label = { Text("Pointer") },
                    colors = FilterChipDefaults.filterChipColors(
                        selectedContainerColor = MaterialTheme.colorScheme.primaryContainer, selectedLabelColor = MaterialTheme.colorScheme.onPrimaryContainer,
                        labelColor = MaterialTheme.colorScheme.onSurface))
                val count = info?.monitors ?: 1
                if (count > 1) FilledTonalButton({
                    poke()
                    monitor = ((info?.monitor ?: monitor) + 1) % count
                    prefs.edit().putInt("stream_monitor", monitor).apply()
                    info = null; scale = 1f; offset = Offset.Zero
                }) { Text("Monitor ${(info?.monitor ?: monitor) + 1}/$count") }
                SingleChoiceSegmentedButtonRow(Modifier.width(300.dp)) {
                    listOf("View", "Direct", "Trackpad").forEachIndexed { idx, label ->
                        SegmentedButton(mode == idx, { poke(); mode = idx; keyboard = false; scale = 1f; offset = Offset.Zero },
                            SegmentedButtonDefaults.itemShape(idx, 3), icon = {}, colors = hadalSegments()) { Text(label, maxLines = 1) }
                    }
                }
                FilledTonalIconButton({ barShown = false }) { Text("✕", style = MaterialTheme.typography.titleMedium) }
            }
        }
        if (!barShown && status == null) BarHandle(::poke, Modifier.align(Alignment.TopEnd).padding(top = 12.dp, end = 12.dp))

        if (controls && keyboard) Box(
            Modifier.align(Alignment.BottomStart).fillMaxWidth().padding(end = 80.dp).background(Color.Black.copy(alpha = 0.8f))
                .safeDrawingPadding().imePadding().padding(8.dp),
        ) { TypeBar(pad) }
    }
}

private enum class Touch { TAP, DRAG, HOLD, PINCH }

// Direct mode: the picture is the PC screen
fun Modifier.directInput(
    key: Any, scale: () -> Float, point: (Offset, IntSize) -> Unit, click: (right: Boolean) -> Unit,
    scroll: (Float) -> Unit, zoom: (Float, Offset) -> Unit,
) = pointerInput(key) {
    awaitEachGesture {
        val down = awaitFirstDown()
        var kind: Touch? = null
        withTimeoutOrNull(450L) {
            while (kind == null) {
                val ev = awaitPointerEvent()
                val c = ev.changes.first()
                kind = when {
                    ev.changes.count { it.pressed } >= 2 -> Touch.PINCH
                    !c.pressed -> Touch.TAP
                    (c.position - down.position).getDistance() > viewConfiguration.touchSlop -> Touch.DRAG
                    else -> null
                }
            }
        }
        when (kind ?: Touch.HOLD) {
            Touch.PINCH -> twoFingers(scale(), viewConfiguration.touchSlop, zoom, scroll)
            Touch.TAP -> { point(down.position, size); click(false) }
            Touch.DRAG -> while (true) {
                val c = awaitPointerEvent().changes.first()
                if (!c.pressed) break
                point(c.position, size)
                c.consume()
            }
            Touch.HOLD -> { point(down.position, size); click(true); waitForUpOrCancellation() }
        }
    }
}

// Trackpad mode: relative pointer
fun Modifier.trackpadInput(
    speed: Float, move: (Float, Float) -> Unit, click: (right: Boolean) -> Unit, scroll: (Float) -> Unit, zoom: (Float) -> Unit,
) = pointerInput(speed) {
    awaitEachGesture {
        val down = awaitFirstDown()
        var travel = 0f
        while (true) {
            val ev = awaitPointerEvent()
            val pressed = ev.changes.filter { it.pressed }
            if (pressed.isEmpty()) {
                if (travel < viewConfiguration.touchSlop && ev.changes.first().uptimeMillis - down.uptimeMillis < 300) click(false)
                break
            }
            if (pressed.size >= 2) {
                if (twoFingers(1f, viewConfiguration.touchSlop, { z, _ -> zoom(z) }, scroll)) click(true)
                break
            }
            val d = pressed.first().positionChange()
            travel += d.getDistance()
            move(d.x * speed, d.y * speed)
            ev.changes.forEach { it.consume() }
        }
    }
}

// Pinch zooms, two finger drag scrolls
suspend fun AwaitPointerEventScope.twoFingers(toScreen: Float, slop: Float, zoom: (Float, Offset) -> Unit, scroll: (Float) -> Unit): Boolean {
    var zooming: Boolean? = null
    var spread = 0f
    var moved = Offset.Zero
    while (true) {
        val ev = awaitPointerEvent()
        if (ev.changes.none { it.pressed }) return zooming == null
        if (ev.changes.count { it.pressed } >= 2) {
            val pan = ev.calculatePan()
            when (zooming) {
                null -> {
                    spread += (ev.calculateCentroidSize(true) - ev.calculateCentroidSize(false)) * toScreen
                    moved += pan * toScreen
                    zooming = if (abs(spread) > slop) true else if (moved.getDistance() > slop) false else null
                }
                true -> zoom(ev.calculateZoom(), pan)
                false -> scroll(pan.y * toScreen)
            }
        }
        ev.changes.forEach { it.consume() }
    }
}

@Composable
fun BarHandle(onClick: () -> Unit, modifier: Modifier = Modifier) =
    FilledTonalIconButton(onClick, modifier.size(40.dp).alpha(0.55f)) { Text("⋯", style = MaterialTheme.typography.titleMedium) }

@Composable
fun ChoiceRow(label: String, key: String, default: Int, options: List<Pair<Int, String>>, unit: String = "") {
    val prefs = Pc.prefs(LocalContext.current)
    var v by remember { mutableIntStateOf(prefs.getInt(key, default)) }
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(label + if (unit.isNotEmpty()) " (${unit.trim()})" else "", style = MaterialTheme.typography.labelLarge)
        SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth()) {
            options.forEachIndexed { idx, (value, text) ->
                SegmentedButton(v == value, { v = value; prefs.edit().putInt(key, value).apply() }, SegmentedButtonDefaults.itemShape(idx, options.size), icon = {}, colors = hadalSegments()) {
                    Text(text)
                }
            }
        }
    }
}

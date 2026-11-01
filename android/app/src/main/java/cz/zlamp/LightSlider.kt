package cz.zlamp

import android.content.Context
import android.graphics.*
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.widget.SeekBar
import kotlin.math.roundToInt

/** A native SeekBar: touch, keyboard, TalkBack and range semantics stay with Android. */
class LightSlider(context: Context, private val kind: String) : SeekBar(context) {
    var tint = Color.rgb(215, 250, 120)
        set(value) { field = value; invalidate() }
    private val density = resources.displayMetrics.density
    private fun dp(value: Int) = (value * density).roundToInt()
    init {
        minimumHeight = dp(56)
        setPadding(dp(12), 0, dp(12), 0)
        splitTrack = false
        progressTintList = null; progressBackgroundTintList = null
        thumbTintList = null
        thumb = GradientDrawable().apply {
            setColor(Color.rgb(244,250,239)); setStroke(dp(2), Color.rgb(36, 47, 44)); cornerRadius = dp(6).toFloat(); setSize(dp(12), dp(34))
        }
        thumbOffset = dp(6)
        progressDrawable = object : Drawable() {
            private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
            override fun draw(canvas: Canvas) {
                val box = RectF(bounds); box.top = box.centerY() - dp(19); box.bottom = box.top + dp(38)
                val radius = dp(19).toFloat()
                val colors = when (kind) {
                    "kelvin" -> intArrayOf(Color.rgb(255, 170, 81), Color.rgb(255, 233, 203), Color.rgb(178, 211, 255))
                    "hue" -> intArrayOf(Color.RED, Color.YELLOW, Color.GREEN, Color.CYAN, Color.BLUE, Color.MAGENTA, Color.RED)
                    "saturation" -> intArrayOf(Color.rgb(230, 224, 218), tint)
                    else -> null
                }
                if (colors != null && box.width() > 0) {
                    paint.shader = LinearGradient(box.left, 0f, box.right, 0f, colors, null, Shader.TileMode.CLAMP)
                    canvas.drawRoundRect(box, radius, radius, paint); paint.shader = null
                } else {
                    paint.color = Color.rgb(29, 42, 43); canvas.drawRoundRect(box, radius, radius, paint)
                    val fraction = (progress - min).toFloat() / (max - min).coerceAtLeast(1)
                    val fill = RectF(box).apply { right = left + dp(38) + (width() - dp(38)).coerceAtLeast(0f) * fraction }
                    paint.shader = LinearGradient(fill.left,fill.top,fill.right,fill.bottom,LightArtwork.mix(tint,Color.BLACK,.5f),LightArtwork.mix(tint,Color.WHITE,.2f),Shader.TileMode.CLAMP)
                    canvas.drawRoundRect(fill, radius, radius, paint); paint.shader = null
                    paint.color = 0x55FFFFFF
                    for (i in 1..9) {
                        val x = box.left+box.width()*i/10f; val mark = dp(if (i==5) 7 else 3).toFloat()
                        canvas.drawRoundRect(x-dp(1)/2f,box.centerY()-mark/2f,x+dp(1)/2f,box.centerY()+mark/2f,1f,1f,paint)
                    }
                }
                paint.style = Paint.Style.STROKE; paint.strokeWidth = density; paint.color = 0x407EABA1
                canvas.drawRoundRect(box,radius,radius,paint); paint.style = Paint.Style.FILL
            }
            override fun setAlpha(alpha: Int) { paint.alpha = alpha }
            override fun setColorFilter(filter: ColorFilter?) { paint.colorFilter = filter }
            override fun onLevelChange(level: Int): Boolean { invalidateSelf(); return true }
            @Suppress("DEPRECATION") override fun getOpacity() = PixelFormat.TRANSLUCENT
        }
    }
}

package cz.zlamp

import android.graphics.*
import android.graphics.drawable.Drawable
import kotlin.math.min

/** Original optical artwork; drawn only when the UI changes, without a background animation. */
class LightArtwork(private val density: Float, private val orb: Boolean = false) : Drawable() {
    var tone: Int = Color.rgb(215, 250, 120)
        set(value) { if (field != value) { field = value; invalidateSelf() } }
    var second: Int = Color.rgb(101, 226, 224)
        set(value) { if (field != value) { field = value; invalidateSelf() } }
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val shape = Path()
    override fun draw(c: Canvas) {
        val box = RectF(bounds); val w = box.width(); val h = box.height()
        if (w <= 0 || h <= 0) return
        c.save(); c.translate(box.left, box.top)
        if (orb) {
            val r = min(w,h)/2f; val cx = w/2f; val cy = h/2f
            paint.shader = RadialGradient(cx,cy,r,intArrayOf(alpha(tone,100),alpha(tone,35),Color.TRANSPARENT),floatArrayOf(0f,.55f,1f),Shader.TileMode.CLAMP)
            c.drawCircle(cx,cy,r,paint); paint.shader = null
            paint.style = Paint.Style.STROKE; paint.strokeWidth = density
            paint.color = alpha(tone,85); c.drawCircle(cx,cy,r*.78f,paint)
            paint.color = alpha(tone,180); c.drawArc(cx-r*.78f,cy-r*.78f,cx+r*.78f,cy+r*.78f,220f,95f,false,paint)
            paint.style = Paint.Style.FILL
            paint.shader = RadialGradient(cx-r*.14f,cy-r*.18f,r*.54f,intArrayOf(mix(tone,Color.WHITE,.65f),tone,mix(tone,Color.BLACK,.7f)),floatArrayOf(0f,.48f,1f),Shader.TileMode.CLAMP)
            c.drawCircle(cx,cy,r*.49f,paint); paint.shader = null
            paint.color = Color.WHITE; c.drawCircle(cx+r*.67f,cy+r*.4f,density*2,paint)
        } else {
            val corner = 28*density
            shape.reset(); shape.addRoundRect(RectF(0f,0f,w,h),corner,corner,Path.Direction.CW); c.clipPath(shape)
            paint.color = Color.rgb(19,24,27); c.drawRect(0f,0f,w,h,paint)
            val cy = min(h*.4f,120*density)
            paint.shader = RadialGradient(w*.96f,cy,w*.85f,intArrayOf(alpha(tone,52),alpha(tone,13),Color.TRANSPARENT),floatArrayOf(0f,.48f,1f),Shader.TileMode.CLAMP)
            c.drawRect(0f,0f,w,h,paint)
            paint.shader = RadialGradient(w*.65f,-20*density,w*.6f,intArrayOf(alpha(second,36),Color.TRANSPARENT),null,Shader.TileMode.CLAMP)
            c.drawRect(0f,0f,w,h,paint); paint.shader = null
            // Fine arcs suggest a lens; they sit behind content and never capture touch.
            paint.style = Paint.Style.STROKE; paint.strokeWidth = density*.65f; paint.color = alpha(tone,24)
            for (i in 0..2) { val r = (83+i*17)*density; c.drawCircle(w+10*density,cy,r,paint) }
            paint.color = Color.rgb(54,63,61); paint.strokeWidth = density
            c.drawRoundRect(RectF(density/2,density/2,w-density/2,h-density/2),corner,corner,paint)
            paint.style = Paint.Style.FILL
            paint.color = tone; c.drawRoundRect(24*density,0f,55*density,2*density,density,density,paint)
        }
        c.restore()
    }
    override fun setAlpha(alpha: Int) { paint.alpha = alpha; invalidateSelf() }
    override fun setColorFilter(filter: ColorFilter?) { paint.colorFilter = filter; invalidateSelf() }
    @Suppress("OVERRIDE_DEPRECATION", "DEPRECATION") override fun getOpacity() = PixelFormat.TRANSLUCENT
    companion object {
        fun alpha(color: Int, value: Int) = Color.argb(value,Color.red(color),Color.green(color),Color.blue(color))
        fun mix(a: Int, b: Int, part: Float): Int = Color.rgb(
            (Color.red(a)*(1-part)+Color.red(b)*part).toInt(),
            (Color.green(a)*(1-part)+Color.green(b)*part).toInt(),
            (Color.blue(a)*(1-part)+Color.blue(b)*part).toInt())
    }
}

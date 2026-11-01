package cz.zlamp

import android.content.Context
import android.graphics.*
import android.view.MotionEvent
import android.view.View
import kotlin.math.*

class ColorWheel(context: Context) : View(context) {
    var hue = 0f
    var saturation = 100f
    var kelvin = 2700
        set(value) { if (field != value) { field = value; updateWhite(); invalidate() } }
    var onColor: ((Int, Int, Boolean) -> Unit)? = null
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private var colors: Shader? = null
    private var center: Shader? = null
    private var pointer = -1
    init { minimumHeight = (240*resources.displayMetrics.density).toInt(); isFocusable = true; contentDescription = "Barevné kolo. Pro přesné hodnoty lze použít posuvníky odstínu a sytosti." }
    override fun onMeasure(w: Int, h: Int) {
        val width = MeasureSpec.getSize(w)
        setMeasuredDimension(width, minOf(width, (270*resources.displayMetrics.density).toInt()))
    }
    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        colors = SweepGradient(w/2f, h/2f, intArrayOf(Color.RED, Color.YELLOW, Color.GREEN, Color.CYAN, Color.BLUE, Color.MAGENTA, Color.RED), null)
        updateWhite()
    }
    private fun updateWhite() {
        val r = min(width, height)/2f-16*resources.displayMetrics.density
        center = if (r > 0) RadialGradient(width/2f, height/2f, r, white(kelvin), Color.TRANSPARENT, Shader.TileMode.CLAMP) else null
    }
    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val cx = width/2f; val cy = height/2f; val r = min(cx, cy)-16*resources.displayMetrics.density
        if (r <= 0) return
        val density = resources.displayMetrics.density
        paint.shader = null; paint.style = Paint.Style.STROKE; paint.strokeWidth = density
        paint.color = Color.rgb(44, 58, 56); canvas.drawCircle(cx,cy,r+5*density,paint)
        paint.strokeCap = Paint.Cap.ROUND
        for (i in 0 until 36) {
            val a = Math.toRadians(i*10.0); val inner = r+8*density; val outer = r+(if (i%3==0) 12 else 10)*density
            paint.color = if (i%3==0) Color.rgb(127,158,149) else Color.rgb(61,82,77)
            canvas.drawLine(cx+sin(a).toFloat()*inner,cy-cos(a).toFloat()*inner,cx+sin(a).toFloat()*outer,cy-cos(a).toFloat()*outer,paint)
        }
        canvas.save(); canvas.rotate(-90f, cx, cy)
        paint.style = Paint.Style.FILL
        paint.shader = colors
        canvas.drawCircle(cx, cy, r, paint); canvas.restore()
        paint.shader = center
        canvas.drawCircle(cx, cy, r, paint); paint.shader = null
        val angle = Math.toRadians(hue.toDouble())
        val x = cx+sin(angle).toFloat()*r*saturation/100; val y = cy-cos(angle).toFloat()*r*saturation/100
        paint.style = Paint.Style.STROKE; paint.strokeWidth = 5f*density; paint.color = 0x99000000.toInt(); canvas.drawCircle(x, y, 10f*density, paint)
        paint.strokeWidth = 2.5f*density; paint.color = Color.WHITE; canvas.drawCircle(x, y, 10f*density, paint); paint.style = Paint.Style.FILL
    }
    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (!isEnabled) return false
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                if (hypot(event.x-width/2f, event.y-height/2f) > min(width, height)/2f) return false
                pointer = event.getPointerId(0); parent?.requestDisallowInterceptTouchEvent(true)
            }
            MotionEvent.ACTION_POINTER_DOWN -> return true
            MotionEvent.ACTION_POINTER_UP -> { if (event.getPointerId(event.actionIndex) != pointer) return true }
        }
        if (pointer < 0) return true
        val i = event.findPointerIndex(pointer)
        if (i >= 0 && event.actionMasked != MotionEvent.ACTION_CANCEL) {
            val dx = event.getX(i)-width/2f; val dy = event.getY(i)-height/2f
            val r = min(width, height)/2f-16*resources.displayMetrics.density
            hue = ((Math.toDegrees(atan2(dx, -dy).toDouble())+360)%360).toFloat()
            saturation = (hypot(dx, dy)/r*100).coerceIn(0f, 100f)
        }
        val done = event.actionMasked == MotionEvent.ACTION_UP || event.actionMasked == MotionEvent.ACTION_CANCEL || event.actionMasked == MotionEvent.ACTION_POINTER_UP
        onColor?.invoke(hue.roundToInt()%360, saturation.roundToInt(), done); invalidate()
        if (done) { pointer = -1; parent?.requestDisallowInterceptTouchEvent(false); if (event.actionMasked == MotionEvent.ACTION_UP) performClick() }
        return true
    }
    override fun performClick(): Boolean { super.performClick(); return true }
    companion object {
        fun white(kelvin: Int): Int {
            val t = kelvin.coerceIn(2000, 6500)/100.0
            val g = (99.4708*ln(t)-161.1196).roundToInt().coerceIn(0, 255)
            val b = (138.5177*ln(t-10)-305.0448).roundToInt().coerceIn(0, 255)
            return Color.rgb(255, g, b)
        }
    }
}

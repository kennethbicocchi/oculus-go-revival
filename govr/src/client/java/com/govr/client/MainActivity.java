package com.govr.client;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import java.nio.ByteBuffer;

/** NativeActivity shell: loads VrApi before the native client library. */
public class MainActivity extends android.app.NativeActivity {
  static {
    System.loadLibrary("vrapi");
    System.loadLibrary("govr");
  }

  /**
   * Renders the on-screen menu / player status text into an RGBA (premultiplied) buffer of
   * w x h pixels, called from native code. Lines are separated by '\n'; a line starting with
   * "▸" is drawn highlighted. Empty text clears the buffer (transparent).
   */
  public static void renderText(String text, int w, int h, ByteBuffer out) {
    Bitmap bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
    Canvas c = new Canvas(bmp);
    c.drawColor(Color.TRANSPARENT);
    if (text != null && !text.isEmpty()) {
      String[] lines = text.split("\n");
      Paint bg = new Paint(Paint.ANTI_ALIAS_FLAG);
      bg.setColor(Color.argb(215, 20, 24, 32));
      c.drawRoundRect(new RectF(4, 4, w - 4, h - 4), 36, 36, bg);
      Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
      p.setTypeface(Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD));
      float size = Math.min(h / (lines.length + 1.2f) * 0.72f, 64f);
      p.setTextSize(size);
      float lineH = size * 1.35f;
      float y = (h - lineH * lines.length) / 2f + size;
      Paint hl = new Paint(Paint.ANTI_ALIAS_FLAG);
      hl.setColor(Color.argb(255, 40, 110, 200));
      for (String line : lines) {
        boolean selected = line.startsWith("▸");
        if (selected) {
          c.drawRoundRect(new RectF(40, y - size * 1.05f, w - 40, y + size * 0.35f), 14, 14, hl);
        }
        p.setColor(selected ? Color.WHITE : Color.argb(255, 225, 230, 238));
        float tw = p.measureText(line);
        c.drawText(line, Math.max(48, (w - tw) / 2f), y, p);
        y += lineH;
      }
    }
    out.rewind();
    bmp.copyPixelsToBuffer(out);
    bmp.recycle();
  }
}

package com.govr.client;

import android.app.AlarmManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.SystemClock;
import android.util.Log;

/**
 * Boot autostart without touching the system image: on BOOT_COMPLETED, start the client once the
 * Oculus shell (and the stock autostart, which opens the browser after ~40 s) have settled, so GoVR
 * ends up on top. Power stays stock (proximity sensor decides): forcing the display on drains the
 * battery faster than USB can charge it.
 */
public class BootReceiver extends BroadcastReceiver {
  static final String TAG = "GoVR";
  static final long START_DELAY_MS = 55_000;

  @Override
  public void onReceive(Context context, Intent intent) {
    Log.i(TAG, "boot: " + intent.getAction());
    Intent start = new Intent(context, MainActivity.class)
        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
    PendingIntent pi = PendingIntent.getActivity(context, 0, start, PendingIntent.FLAG_UPDATE_CURRENT);
    AlarmManager am = (AlarmManager) context.getSystemService(Context.ALARM_SERVICE);
    am.setExact(AlarmManager.ELAPSED_REALTIME_WAKEUP, SystemClock.elapsedRealtime() + START_DELAY_MS, pi);
    Log.i(TAG, "boot: client start scheduled in " + START_DELAY_MS / 1000 + " s");
  }
}

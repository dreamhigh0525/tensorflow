/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
package ovic.demo.app;

import android.app.Activity;
import android.content.res.AssetFileDescriptor;
import android.content.res.AssetManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Bundle;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;
import android.view.View;
import android.widget.TextView;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.nio.MappedByteBuffer;
import java.nio.channels.FileChannel;
import java.text.DecimalFormat;
import org.tensorflow.ovic.OvicSingleImageResult;

/** Class that benchmark image classifier models. */
public class OvicBenchmarkerActivity extends Activity {
  /** Tag for the {@link Log}. */
  private static final String TAG = "OvicBenchmarkerActivity";

  /** Name of the label file stored in Assets. */
  private static final String LABEL_PATH = "labels.txt";

  private static final String TEST_IMAGE_PATH = "test_image_224.jpg";
  private static final String MODEL_PATH = "float_model.lite";
  /**
   * Each bottom press will launch a benchmarking experiment. The experiment stops when either the
   * total native latency reaches WALL_TIME or the number of iterations reaches MAX_ITERATIONS,
   * whichever comes first.
   */
  /** Wall time for each benchmarking experiment. */
  private static final double WALL_TIME = 3000;
  /** Maximum number of iterations in each benchmarking experiment. */
  private static final int MAX_ITERATIONS = 100;
  /** Mask for binding to a single big core. Pixel 1 (4), Pixel 2 (16). */
  private static final int BIG_CORE_MASK = 16;
  /** Amount of time in milliseconds to wait for affinity to set. */
  private static final int WAIT_TIME_FOR_AFFINITY = 1000;

  /* The model to be benchmarked. */
  private MappedByteBuffer model = null;
  private InputStream labelInputStream = null;
  private OvicBenchmarker benchmarker;
  /** Inference result of each iteration. */
  OvicSingleImageResult iterResult = null;

  private TextView textView = null;
  // private Button startButton = null;
  private static final DecimalFormat df2 = new DecimalFormat(".##");

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    setContentView(R.layout.activity_main);

    // TextView used to display the progress, for information purposes only.
    textView = (TextView) findViewById(R.id.textView);
  }

  private Bitmap loadTestBitmap() throws IOException {
    InputStream imageStream = getAssets().open(TEST_IMAGE_PATH);
    return BitmapFactory.decodeStream(imageStream);
  }

  public void initializeTest() throws IOException {
    Log.i(TAG, "Initializing benchmarker.");
    benchmarker = new OvicBenchmarker(WALL_TIME);
    AssetManager am = getAssets();
    AssetFileDescriptor fileDescriptor = am.openFd(MODEL_PATH);
    FileInputStream modelInputStream = new FileInputStream(fileDescriptor.getFileDescriptor());
    FileChannel fileChannel = modelInputStream.getChannel();
    long startOffset = fileDescriptor.getStartOffset();
    long declaredLength = fileDescriptor.getDeclaredLength();
    model = fileChannel.map(FileChannel.MapMode.READ_ONLY, startOffset, declaredLength);
    labelInputStream = am.open(LABEL_PATH);
  }

  public Boolean doTestIteration() throws IOException, InterruptedException {
    if (benchmarker == null) {
      throw new RuntimeException("Benchmarker has not been initialized.");
    }
    if (benchmarker.shouldStop()) {
      return false;
    }
    if (!benchmarker.readyToTest()) {
      Log.i(TAG, "getting ready to test.");
      benchmarker.getReadyToTest(labelInputStream, model);
      if (!benchmarker.readyToTest()) {
        throw new RuntimeException("Failed to get the benchmarker ready.");
      }
    }
    Log.i(TAG, "Going to do test iter.");
    // Start testing.
    Bitmap testImageBitmap = loadTestBitmap();
    iterResult = benchmarker.doTestIteration(testImageBitmap);
    testImageBitmap.recycle();
    if (iterResult == null) {
      throw new RuntimeException("Inference failed to produce a result.");
    }
    Log.i(TAG, iterResult.toString());
    return true;
  }

  public void startPressed(View view) throws IOException {
    Log.i(TAG, "Start pressed");
    try {
      initializeTest();
    } catch (IOException e) {
      Log.e(TAG, "Can't initialize benchmarker.", e);
      throw e;
    }
    String displayText = "";
    try {
      setProcessorAffinity(BIG_CORE_MASK);
    } catch (IOException e) {
      Log.e(TAG, e.getMessage());
      displayText = e.getMessage() + "\n";
    }
    Log.i(TAG, "Successfully initialized benchmarker.");
    int testIter = 0;
    Boolean iterSuccess = false;
    double totalLatency = 0.0f;
    while (testIter < MAX_ITERATIONS) {
      try {
        iterSuccess = doTestIteration();
      } catch (IOException e) {
        Log.e(TAG, "Error during iteration " + testIter);
        throw e;
      } catch (InterruptedException e) {
        Log.e(TAG, "Interrupted at iteration " + testIter);
      }
      if (!iterSuccess) {
        break;
      }
      testIter++;
      totalLatency += (double) iterResult.latency;
    }
    ;
    Log.i(TAG, "Benchmarking finished");

    if (textView != null) {
      if (testIter > 0) {
        textView.setText(
            displayText
                + MODEL_PATH
                + ": Average latency="
                + df2.format(totalLatency / testIter)
                + "ms after "
                + testIter
                + " runs.");
      } else {
        textView.setText("Benchmarker failed to run on more than one images.");
      }
    }
  }

  private static void setProcessorAffinity(int mask) throws IOException {
    int myPid = Process.myPid();
    Log.i(TAG, String.format("Setting processor affinity to 0x%02x", mask));

    String command = String.format("taskset -a -p %x %d", mask, myPid);
    try {
      Runtime.getRuntime().exec(command).waitFor();
    } catch (InterruptedException e) {
      throw new IOException("Interrupted: " + e);
    }

    // Make sure set took effect - try for a second to confirm the change took.  If not then fail.
    long startTimeMs = SystemClock.elapsedRealtime();
    while (true) {
      int readBackMask = readCpusAllowedMask();
      if (readBackMask == mask) {
        Log.i(TAG, String.format("Successfully set affinity to 0x%02x", mask));
        return;
      }
      if (SystemClock.elapsedRealtime() > startTimeMs + WAIT_TIME_FOR_AFFINITY) {
        throw new IOException(
            String.format(
                "Core-binding failed: affinity set to 0x%02x but read back as 0x%02x\n"
                    + "please root device.",
                mask, readBackMask));
      }

      try {
        Thread.sleep(50);
      } catch (InterruptedException e) {
        // Ignore sleep interrupted, will sleep again and compare is final cross-check.
      }
    }
  }

  public static int readCpusAllowedMask() throws IOException {
    // Determine how many CPUs there are total
    final String pathname = "/proc/self/status";
    final String resultPrefix = "Cpus_allowed:";
    File file = new File(pathname);
    String line = "<NO LINE READ>";
    String allowedCPU = "";
    Integer allowedMask = null;
    BufferedReader bufReader = null;
    try {
      bufReader = new BufferedReader(new FileReader(file));
      while ((line = bufReader.readLine()) != null) {
        if (line.startsWith(resultPrefix)) {
          allowedMask = Integer.valueOf(line.substring(resultPrefix.length()).trim(), 16);
          allowedCPU = bufReader.readLine();
          break;
        }
      }
    } catch (RuntimeException e) {
      throw new IOException(
          "Invalid number in " + pathname + " line: \"" + line + "\": " + e.getMessage());
    } finally {
      if (bufReader != null) {
        bufReader.close();
      }
    }
    if (allowedMask == null) {
      throw new IOException(pathname + " missing " + resultPrefix + " line");
    }
    Log.i(TAG, allowedCPU);
    return allowedMask;
  }
}

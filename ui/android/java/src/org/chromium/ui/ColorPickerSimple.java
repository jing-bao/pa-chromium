// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
package org.chromium.ui;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.View;


/**
 * Draws a grid of (predefined) colors and allows the user to choose one of
 * those colors.
 */
public class ColorPickerSimple extends View {
    private static final int ROW_COUNT = 2;

    private static final int COLUMN_COUNT = 4;

    private static final int GRID_CELL_COUNT = ROW_COUNT * COLUMN_COUNT;

    private static final int[] COLORS = { Color.RED,
                                          Color.CYAN,
                                          Color.BLUE,
                                          Color.GREEN,
                                          Color.MAGENTA,
                                          Color.YELLOW,
                                          Color.BLACK,
                                          Color.WHITE
                                        };

    private Rect[] mBounds;

    private Paint[] mPaints;

    private OnColorChangedListener mOnColorTouchedListener;

    public ColorPickerSimple(Context context) {
        super(context);
    }

    public ColorPickerSimple(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    public ColorPickerSimple(Context context, AttributeSet attrs, int defStyle) {
        super(context, attrs, defStyle);
    }

    /**
     * Initializes the listener and precalculates the grid and color positions.
     *
     * @param onColorChangedListener The listener that gets notified when the user touches
     *                               a color.
     */
    public void init(OnColorChangedListener onColorChangedListener) {
        mOnColorTouchedListener = onColorChangedListener;

        // This will get calculated when the layout size is updated.
        mBounds = null;

        mPaints = new Paint[GRID_CELL_COUNT];
        for (int i = 0; i < GRID_CELL_COUNT; ++i) {
            Paint newPaint = new Paint();
            newPaint.setColor(COLORS[i]);
            mPaints[i] = newPaint;
        }
    }

    /**
     * Draws the grid of colors, based on the rectangles calculated in onSizeChanged().
     *
     * @param canvas The canvas the colors are drawn onto.
     */
    @Override
    public void onDraw(Canvas canvas) {
        if (mBounds == null || mPaints == null) {
            return;
        }
        for (int i = 0; i < GRID_CELL_COUNT; ++i) {
            canvas.drawRect(mBounds[i], mPaints[i]);
        }
    }

    /**
     * Responds to the user touching the grid and works out which color has been chosen as
     * a result, depending on the X,Y coordinate.
     *
     * @param event The MotionEvent the X,Y coordinates are retrieved from.
     */
    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (event.getAction() != MotionEvent.ACTION_DOWN ||
            mOnColorTouchedListener == null) {
            return false;
        }

        if ((getWidth() > 0) && (getHeight() > 0)) {
            int x = (int) event.getX();
            int y = (int) event.getY();

            int column = x * COLUMN_COUNT / getWidth();
            int row = y * ROW_COUNT / getHeight();

            int colorIndex = (row * COLUMN_COUNT) + column;
            if (colorIndex >= 0 && colorIndex < COLORS.length) {
                mOnColorTouchedListener.onColorChanged(COLORS[colorIndex]);
            }
        }
        return true;
    }

    /**
     * Recalculates the color grid with the new sizes.
     */
    @Override
    protected void onSizeChanged(int width, int height, int oldw, int oldh) {
        calculateGrid(width, height);
    }

    /**
     * Calculates the sizes and positions of the cells in the grid, splitting
     * them up as evenly as possible.
     */
    private void calculateGrid(final int width, final int height) {
        mBounds = new Rect[GRID_CELL_COUNT];

        for (int i = 0; i < ROW_COUNT; ++i) {
            for (int j = 0; j < COLUMN_COUNT; ++j) {
                int left = j * width / COLUMN_COUNT;
                int right = (j + 1) * width / COLUMN_COUNT;

                int top = i * height / ROW_COUNT;
                int bottom = (i + 1) * height / ROW_COUNT;

                Rect rect = new Rect(left, top, right, bottom);
                mBounds[(i * COLUMN_COUNT) + j] = rect;
            }
        }
    }
}

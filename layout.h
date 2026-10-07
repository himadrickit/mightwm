#pragma once

typedef struct {
	int left, top, right, bottom;
} LRect;

typedef enum {
	LAYOUT_MASTER_STACK = 0,
	LAYOUT_GRID,
	LAYOUT_COLUMNS
} LayoutKind;

LRect layoutInner(LRect area, int gap);
void layoutCompute(LayoutKind kind, LRect area, int count, int gap, int masterPercent, LRect* out);
int layoutDividerX(LRect area, int gap, int masterPercent);
int layoutPercentFromDivider(LRect area, int gap, int dividerX);

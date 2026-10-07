#include "layout.h"

#define MIN_PERCENT 10
#define MAX_PERCENT 90

static int clampInt(int value, int low, int high)
{
	if (value < low) return low;
	if (value > high) return high;
	return value;
}

// Computes the i-th of n equal spans inside [start, start + length) separated by gap pixels.
// The remainder pixels go to the first spans, so the spans always end flush with the area.
static void spanAt(int start, int length, int n, int gap, int i, int* low, int* high)
{
	int available = length - gap * (n - 1);
	if (available < n) {
		available = n;
	}

	int base = available / n;
	int extra = available % n;
	int size = base + (i < extra ? 1 : 0);

	*low = start + i * (base + gap) + (i < extra ? i : extra);
	*high = *low + size;
}

LRect layoutInner(LRect area, int gap)
{
	LRect inner = { area.left + gap, area.top + gap, area.right - gap, area.bottom - gap };

	if (inner.right <= inner.left) inner.right = inner.left + 1;
	if (inner.bottom <= inner.top) inner.bottom = inner.top + 1;

	return inner;
}

static int masterWidth(LRect inner, int gap, int masterPercent)
{
	int width = inner.right - inner.left;
	int usable = width - gap;
	int masterWidth = usable * clampInt(masterPercent, MIN_PERCENT, MAX_PERCENT) / 100;

	return clampInt(masterWidth, 1, usable - 1);
}

int layoutDividerX(LRect area, int gap, int masterPercent)
{
	LRect inner = layoutInner(area, gap);
	return inner.left + masterWidth(inner, gap, masterPercent);
}

int layoutPercentFromDivider(LRect area, int gap, int dividerX)
{
	LRect inner = layoutInner(area, gap);
	int usable = inner.right - inner.left - gap;

	if (usable <= 0) {
		return 50;
	}

	return clampInt((dividerX - inner.left) * 100 / usable, MIN_PERCENT, MAX_PERCENT);
}

static void computeColumns(LRect inner, int count, int gap, LRect* out)
{
	for (int i = 0; i < count; i++) {
		out[i].top = inner.top;
		out[i].bottom = inner.bottom;
		spanAt(inner.left, inner.right - inner.left, count, gap, i, &out[i].left, &out[i].right);
	}
}

static void computeMasterStack(LRect inner, int count, int gap, int masterPercent, LRect* out)
{
	if (inner.right - inner.left - gap < 2) {
		computeColumns(inner, count, gap, out);
		return;
	}

	int masterRight = inner.left + masterWidth(inner, gap, masterPercent);

	out[0].left = inner.left;
	out[0].top = inner.top;
	out[0].right = masterRight;
	out[0].bottom = inner.bottom;

	int stackCount = count - 1;
	for (int i = 0; i < stackCount; i++) {
		out[i + 1].left = masterRight + gap;
		out[i + 1].right = inner.right;
		spanAt(inner.top, inner.bottom - inner.top, stackCount, gap, i, &out[i + 1].top, &out[i + 1].bottom);
	}
}

static void computeGrid(LRect inner, int count, int gap, LRect* out)
{
	int columns = 1;
	while (columns * columns < count) {
		columns++;
	}
	int rows = (count + columns - 1) / columns;

	for (int row = 0; row < rows; row++) {
		int inRow = (row < rows - 1) ? columns : count - (rows - 1) * columns;
		int top, bottom;
		spanAt(inner.top, inner.bottom - inner.top, rows, gap, row, &top, &bottom);

		for (int column = 0; column < inRow; column++) {
			LRect* cell = &out[row * columns + column];
			cell->top = top;
			cell->bottom = bottom;
			spanAt(inner.left, inner.right - inner.left, inRow, gap, column, &cell->left, &cell->right);
		}
	}
}

void layoutCompute(LayoutKind kind, LRect area, int count, int gap, int masterPercent, LRect* out)
{
	if (count <= 0) {
		return;
	}

	if (gap < 0) gap = 0;

	LRect inner = layoutInner(area, gap);

	if (count == 1) {
		out[0] = inner;
		return;
	}

	switch (kind) {
		case LAYOUT_GRID:
			computeGrid(inner, count, gap, out);
			break;
		case LAYOUT_COLUMNS:
			computeColumns(inner, count, gap, out);
			break;
		default:
			computeMasterStack(inner, count, gap, masterPercent, out);
			break;
	}
}

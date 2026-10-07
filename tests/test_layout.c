#include "../layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; if (failures < 15) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static void verify(LayoutKind kind, LRect area, int n, int gap, int pct)
{
	LRect out[64];
	memset(out, 0, sizeof out);
	layoutCompute(kind, area, n, gap, pct, out);
	LRect inner = layoutInner(area, gap);

	int minL = 1 << 30, minT = 1 << 30, maxR = -(1 << 30), maxB = -(1 << 30);
	for (int i = 0; i < n; i++) {
		CHECK(out[i].right > out[i].left && out[i].bottom > out[i].top, "kind %d n %d: empty rect %d", kind, n, i);
		CHECK(out[i].left >= inner.left && out[i].right <= inner.right && out[i].top >= inner.top && out[i].bottom <= inner.bottom,
			"kind %d n %d gap %d: rect %d outside inner area", kind, n, gap, i);
		if (out[i].left < minL) minL = out[i].left;
		if (out[i].top < minT) minT = out[i].top;
		if (out[i].right > maxR) maxR = out[i].right;
		if (out[i].bottom > maxB) maxB = out[i].bottom;

		for (int j = i + 1; j < n; j++) {
			int overlapX = out[i].left < out[j].right && out[j].left < out[i].right;
			int overlapY = out[i].top < out[j].bottom && out[j].top < out[i].bottom;
			CHECK(!(overlapX && overlapY), "kind %d n %d gap %d: rects %d and %d overlap", kind, n, gap, i, j);
		}

		// Nearest neighbour to the right / below must be exactly `gap` away: no slack pixels.
		int nearestRight = 1 << 30, nearestBelow = 1 << 30;
		for (int j = 0; j < n; j++) {
			if (j == i) continue;
			int overlapX = out[i].left < out[j].right && out[j].left < out[i].right;
			int overlapY = out[i].top < out[j].bottom && out[j].top < out[i].bottom;
			if (overlapY && out[j].left >= out[i].right && out[j].left - out[i].right < nearestRight) nearestRight = out[j].left - out[i].right;
			if (overlapX && out[j].top >= out[i].bottom && out[j].top - out[i].bottom < nearestBelow) nearestBelow = out[j].top - out[i].bottom;
		}
		CHECK(nearestRight == (1 << 30) || nearestRight == gap, "kind %d n %d gap %d: rect %d right gap %d", kind, n, gap, i, nearestRight);
		CHECK(nearestBelow == (1 << 30) || nearestBelow == gap, "kind %d n %d gap %d: rect %d below gap %d", kind, n, gap, i, nearestBelow);
	}

	// Windows must touch all four edges of the inner area: nothing left over.
	CHECK(minL == inner.left && minT == inner.top && maxR == inner.right && maxB == inner.bottom,
		"kind %d n %d gap %d: not flush (%d,%d,%d,%d) vs (%d,%d,%d,%d)", kind, n, gap,
		minL, minT, maxR, maxB, inner.left, inner.top, inner.right, inner.bottom);
}

int main(void)
{
	srand(7);
	int cases = 0;
	for (int kind = 0; kind < 3; kind++) {
		for (int n = 1; n <= 24; n++) {
			for (int gap = 0; gap <= 12; gap += 3) {
				for (int t = 0; t < 40; t++) {
					LRect area = { rand() % 2000 - 1000, rand() % 500, 0, 0 };
					area.right = area.left + 700 + rand() % 3200;
					area.bottom = area.top + 500 + rand() % 1700;
					verify((LayoutKind)kind, area, n, gap, 10 + rand() % 81);
					cases++;
				}
			}
		}
	}

	LRect area = { 0, 0, 1920, 1040 };
	int pct = layoutPercentFromDivider(area, 6, layoutDividerX(area, 6, 63));
	CHECK(pct >= 62 && pct <= 64, "divider round trip gave %d", pct);

	printf("%d layout cases, %d failures\n", cases, failures);
	return failures != 0;
}

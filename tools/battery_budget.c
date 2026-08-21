/*
 * Battery runtime for a TriageBox sensor node.
 *
 *   cc -O2 -Wall -Wextra -lm -o /tmp/tb_batt tools/battery_budget.c && /tmp/tb_batt
 *
 * WHY THIS EXISTS. The proposal claims 11 hours on 2x 18650 3500mAh. The build is
 * moving to a single 3.7V LiPo, and a single cell holds LESS THAN HALF the energy
 * of two 18650s -- so the claim does not survive the change unless the load also
 * drops. That is worth knowing before it appears on a slide.
 *
 * WHAT THIS IS NOT. It does not know the node's actual current draw, because
 * nobody has measured it. Every per-rail figure below is a DATASHEET TYPICAL,
 * which is reliably wrong in the optimistic direction: it excludes regulator
 * loss, LED brightness, and whatever the display backlight is set to. The
 * structure is what is useful -- measure one number (average current at the
 * battery terminals) and this converts it to runtime honestly.
 *
 * Run it, then run the measurement in docs/pengujian-lapangan.md, then put the
 * MEASURED row on the slide and delete the estimate.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>

/* LiPo nominal. Runtime scales with energy (Wh), not with mAh, which is why
 * comparing a 2-cell pack to a 1-cell pack on mAh alone is the trap. */
#define LIPO_NOMINAL_V 3.7

/*
 * Usable fraction of rated capacity. Not 100%: a LiPo is empty at ~3.0V under
 * load, the protection circuit cuts earlier than that, and a regulator with a
 * dropout stops holding 3.3V before the cell is flat. 85% is the conventional
 * planning figure and is still optimistic on a cold day.
 */
#define USABLE_FRACTION 0.85

struct load {
	const char *name;
	double active_ma;   /* draw while doing its job */
	double duty;        /* fraction of wall-clock time it is active */
	const char *source; /* where the number came from */
};

/*
 * The node's rails, from the schematic implied by the firmware. Every figure is
 * a datasheet typical unless marked, and the duty cycles ARE derived from the
 * firmware rather than guessed -- those are the trustworthy part.
 */
static const struct load LOADS[] = {
	{ "STM32F411 @100MHz", 32.0, 1.00, "datasheet run, 100MHz, periph on" },
	{ "ESP32 (scoring+LCD)", 160.0, 1.00, "typical active, no WiFi TX" },
	{ "LCD backlight", 60.0, 1.00, "assumed 2in TFT at mid brightness" },
	{ "MAX30102 PPG", 1.2, 1.00, "LEDs at 2048 ADC range, continuous" },
	{ "AD8232 ECG", 0.2, 1.00, "datasheet typical" },
	{ "Microphone preamp", 1.0, 1.00, "assumed analog mic + opamp" },
	/* Request-scoped: Pn532_Service blocks ~120ms and only runs on an ESP32
	 * START_SCAN. Assume one scan per patient per minute -- generous. */
	{ "PN532 RFID (scan)", 120.0, 0.002, "120ms per scan, ~1 scan/min" },
	/* SX1278 RX continuous is the node's resting state: it sits in receive
	 * waiting for a poll, all the time. This is the one people forget. */
	{ "SX1278 RX (continuous)", 11.0, 1.00, "datasheet RX cont, LNA boost" },
	/* TX is one 82ms reply per 15s cycle = 0.55% duty. Tiny, and the reason
	 * "turn the radio up" is not a battery argument. */
	{ "SX1278 TX 17dBm", 90.0, 0.0055, "82ms per 15s poll cycle" },
};

/**
 * Runtime in hours.
 *
 * @param mah        cell rating
 * @param load_ma    average draw at the REGULATED rail
 * @param reg_eff    regulator efficiency, 0..1 (a buck is ~0.90; an LDO from
 *                   3.7V to 3.3V is ~0.89 by ratio, from 7.4V only ~0.45)
 */
static double runtime_h(double mah, double load_ma, double reg_eff)
{
	double usable_mah = mah * USABLE_FRACTION;
	double drawn_ma = load_ma / reg_eff;

	return usable_mah / drawn_ma;
}

int main(void)
{
	printf("=== TriageBox node battery budget ===\n\n");

	printf("Per-rail estimate (DATASHEET TYPICALS -- measure before quoting):\n");
	double total = 0.0;
	for (size_t i = 0; i < sizeof(LOADS) / sizeof(LOADS[0]); i++) {
		double avg = LOADS[i].active_ma * LOADS[i].duty;

		total += avg;
		printf("  %-24s %6.1f mA x %6.2f%% = %6.2f mA   %s\n", LOADS[i].name,
		       LOADS[i].active_ma, LOADS[i].duty * 100.0, avg, LOADS[i].source);
	}
	printf("  %-24s %25.2f mA at the 3.3V rail\n\n", "TOTAL", total);

	printf("The radio is 12 mA of that -- 4%%. Turning the LoRa power up or down\n");
	printf("is NOT a battery decision on this board; the ESP32 and the backlight are.\n\n");

	/*
	 * THE ENERGY COMPARISON, which is the part that does not depend on any
	 * estimate above. Runtime scales with watt-hours at a fixed load, so the
	 * ratio between packs is exact even when the absolute hours are not.
	 */
	printf("=== What changing the pack does, independent of the load ===\n\n");
	double wh_2x18650 = 2 * 3.5 * LIPO_NOMINAL_V;
	printf("  2x 18650 3500mAh (the proposal's pack) : %5.1f Wh   100%%\n", wh_2x18650);
	static const double cells[] = { 3000.0, 4000.0, 5000.0 };
	for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
		double wh = cells[i] / 1000.0 * LIPO_NOMINAL_V;

		printf("  1x LiPo %.0fmAh %-24s : %5.1f Wh   %3.0f%%  -> %4.1f h if 11 h was real\n",
		       cells[i], "", wh, wh / wh_2x18650 * 100.0,
		       11.0 * wh / wh_2x18650);
	}
	printf("\n  A single 3000mAh cell is 43%% of the energy of two 18650s. The 11-hour\n");
	printf("  claim becomes 4.7 h at the same load -- the pack change alone breaks it.\n");
	printf("  5000mAh recovers it to 7.8 h. Nothing on one cell reaches 11 h at this\n");
	printf("  load; that needs ~7000mAh or a lower load.\n\n");

	printf("=== Runtime at the estimated load ===\n\n");
	printf("  %-14s %10s %10s %10s\n", "pack", "buck 90%", "LDO 3.7V", "note");
	for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
		double buck = runtime_h(cells[i], total, 0.90);
		double ldo = runtime_h(cells[i], total, 3.3 / 3.7);

		printf("  1x %.0fmAh %7s %8.1f h %9.1f h  %s\n", cells[i], "", buck, ldo,
		       buck >= 8.0 ? "covers a shift" : "under a shift");
	}
	printf("\n  Regulator topology matters as much as 1000mAh of cell. Worth knowing:\n");
	printf("  an LDO dropping 7.4V (2 cells in series) to 3.3V wastes 55%%, so if the\n");
	printf("  18650 pack was in SERIES through an LDO, the single cell is MORE\n");
	printf("  efficient and the real loss is smaller than the energy ratio suggests.\n");
	printf("  Check which topology the 11-hour figure was measured on.\n\n");

	/*
	 * THE TWO METHODS DISAGREE, AND THAT IS THE POINT.
	 *
	 * Scaling the 11-hour claim by energy gives 4.7h on 3000mAh. Building the load
	 * up from datasheets gives 8.6h on the same cell. Both cannot be right, and
	 * the gap between them is the honest measure of how much this file should be
	 * trusted: not much. Printing the implied load side by side turns "the numbers
	 * disagree" into a specific question someone can answer with a multimeter.
	 */
	printf("=== The two methods disagree -- read this before quoting either ===\n\n");
	double implied_ma = (wh_2x18650 * USABLE_FRACTION) / 11.0 / 3.3 * 1000.0 * 0.90;
	printf("  Load implied by the 11-hour claim : %6.0f mA at the 3.3V rail\n",
	       implied_ma);
	printf("  Load estimated from datasheets    : %6.0f mA\n", total);
	printf("  Ratio                             : %6.1fx\n\n", implied_ma / total);
	printf("  One of three things is true, and only a measurement says which:\n");
	printf("    a) the real load is ~%.0f mA and this estimate is missing something\n",
	       implied_ma);
	printf("       big -- most likely a brighter backlight, WiFi on the ESP32, or\n");
	printf("       regulator loss nobody counted;\n");
	printf("    b) the 11-hour figure was conservative and the pack would really\n");
	printf("       have lasted ~%.0f h, in which case a 5000mAh cell gives ~%.0f h;\n",
	       runtime_h(7000.0, total, 0.90), runtime_h(5000.0, total, 0.90));
	printf("    c) the 11-hour figure was never measured at all.\n\n");
	printf("  DO NOT put either table on a slide. Measure the average current at the\n");
	printf("  battery terminals over 10 minutes of normal operation (procedure in\n");
	printf("  docs/pengujian-lapangan.md), then quote one number with its method.\n\n");

	printf("=== What actually buys runtime here ===\n\n");
	struct { const char *what; double saved_ma; } wins[] = {
		{ "LCD backlight off when idle (or a button to wake)", 60.0 },
		{ "ESP32 light-sleep between 1.28s PPG blocks", 80.0 },
		{ "MAX30102 LEDs only while a finger is present", 1.0 },
		{ "SX1278 to RX-single instead of RX-continuous", 8.0 },
	};
	for (size_t i = 0; i < sizeof(wins) / sizeof(wins[0]); i++) {
		double t = total - wins[i].saved_ma;

		printf("  -%5.1f mA  %-48s -> %.1f h on 5000mAh\n", wins[i].saved_ma,
		       wins[i].what, runtime_h(5000.0, t, 0.90));
	}
	printf("\n  The backlight and the ESP32 are the whole budget. Radio tuning is noise.\n");
	printf("  RX-continuous -> RX-single is NOT free: the node would have to know when\n");
	printf("  its poll is due, which the current protocol does not tell it.\n\n");

	/* Self-checks: the arithmetic, not the estimates. */
	assert(fabs(wh_2x18650 - 25.9) < 0.01);
	assert(fabs(3.0 * LIPO_NOMINAL_V / wh_2x18650 - 0.4285714) < 1e-4);
	/* Halving the load must double the runtime. */
	assert(fabs(runtime_h(3000.0, 100.0, 1.0) / runtime_h(3000.0, 200.0, 1.0) - 2.0)
	       < 1e-9);
	/* A worse regulator must never report a longer runtime. */
	assert(runtime_h(3000.0, 100.0, 0.5) < runtime_h(3000.0, 100.0, 0.9));
	/* The radio really is a rounding error on this board. */
	assert((11.0 * 1.0 + 90.0 * 0.0055) / total < 0.06);

	printf("battery_budget: all self-checks passed\n");
	return 0;
}

/*
 * LoRa airtime, duty cycle and link budget for the TriageBox poll cycle.
 *
 *   cc -O2 -Wall -Wextra -lm -o /tmp/tb_radio tools/lora_budget.c && /tmp/tb_radio
 *
 * WHY THIS EXISTS. The radio settings were inherited from the node library's
 * newLoRa() constructor defaults -- nobody chose SF7/BW125/CR4-5, it is just what
 * the library happened to initialise. Every question that matters about them
 * ("can we reach further?", "can we poll faster?", "is 20dBm even legal?") is
 * arithmetic, and arithmetic that nobody had done. Guessing at radio settings
 * fails silently: a slot that is 10ms too short, or a duty cycle over the PA's
 * rating, produces lost packets and dead hardware, not an error message.
 *
 * Every formula here is from the SX1276/77/78/79 datasheet (rev 4, sections 4.1.1
 * and 6.4). The three airtime figures already recorded in docs/lora-air-protocol.md
 * -- ~30ms poll, ~51ms empty vital, ~82ms tagged vital -- are asserted at the end,
 * so if this tool and the documented link disagree, the build fails rather than
 * quietly producing a different answer.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>

/* Packet sizes on this link. lora_poll.h / lora_vital.h are the source. */
#define POLL_BYTES 5u
#define VITAL_MIN_BYTES 18u /* LORA_VITAL_FIXED_LEN, no tag scanned */
#define VITAL_MAX_BYTES 38u /* + LORA_VITAL_RFID_MAX (20) */

#define NODES 20u
#define PERIOD_MS 15000u
/* LORA_POLL_SLOT_MS from lora_poll.h. The station waits this per node whether
 * the airtime needs it or not, so it -- not the airtime -- sets the cycle length
 * at low SF. */
#define LORA_POLL_SLOT_MS 250u

/* Antennas actually fitted: 8dBi on the station, 3dBi on the nodes. */
#define G_STATION_DBI 8.0
#define G_NODE_DBI 3.0

/* SX127x receiver noise figure. 6dB is the figure the sensitivity tables in the
 * datasheet imply at BW125 and is what every link-budget spreadsheet uses. */
#define NOISE_FIGURE_DB 6.0

#define FREQ_MHZ 433.0

/**
 * Demodulator SNR floor per spreading factor, dB (datasheet table 13).
 * This is what buys range: each SF step down the list needs 2.5dB less signal.
 */
static double snr_limit_db(unsigned sf)
{
	static const double snr[] = { -7.5, -10.0, -12.5, -15.0, -17.5, -20.0 };
	assert(sf >= 7 && sf <= 12);
	return snr[sf - 7];
}

/** Thermal noise floor in the modem bandwidth, dBm. */
static double noise_floor_dbm(double bw_hz)
{
	return -174.0 + 10.0 * log10(bw_hz) + NOISE_FIGURE_DB;
}

/** Receiver sensitivity, dBm. */
static double sensitivity_dbm(unsigned sf, double bw_hz)
{
	return noise_floor_dbm(bw_hz) + snr_limit_db(sf);
}

/**
 * Time on air, milliseconds. Datasheet 4.1.1.
 *
 * @param sf   spreading factor 7..12
 * @param bw_hz bandwidth in Hz
 * @param cr   1..4 for coding rates 4/5..4/8
 * @param pl   payload bytes
 * @param preamble preamble symbols
 * @param crc_on   payload CRC appended (adds 16 bits)
 * @param implicit_header  header omitted (saves 20 bits)
 */
static double time_on_air_ms(unsigned sf, double bw_hz, unsigned cr, unsigned pl,
			     unsigned preamble, int crc_on, int implicit_header)
{
	double tsym_ms = (double) (1u << sf) / bw_hz * 1000.0;
	double tpreamble_ms = ((double) preamble + 4.25) * tsym_ms;

	/* Low Data Rate Optimize is mandatory when a symbol exceeds 16ms, and it
	 * costs two of the bits available per symbol. Deriving it here rather than
	 * taking it as a parameter is the point: both firmwares compute it from
	 * SF and BW, so a hand-set value would be a third opinion. */
	int de = (tsym_ms > 16.0) ? 1 : 0;

	double num = 8.0 * pl - 4.0 * sf + 28.0 + (crc_on ? 16.0 : 0.0)
		     - (implicit_header ? 20.0 : 0.0);
	double den = 4.0 * (sf - 2.0 * de);
	double symbols = ceil(num / den) * (cr + 4);

	if (symbols < 0.0) {
		symbols = 0.0;
	}
	return tpreamble_ms + (8.0 + symbols) * tsym_ms;
}

/**
 * Largest path loss the link can absorb, dB. Both antenna gains apply in both
 * directions, so the only asymmetry is the transmit power at each end.
 */
static double max_path_loss_db(double ptx_dbm, unsigned sf, double bw_hz)
{
	return ptx_dbm + G_STATION_DBI + G_NODE_DBI - sensitivity_dbm(sf, bw_hz);
}

/**
 * Free-space range for a given path loss, km. Reported only as a sanity anchor:
 * at 433MHz with these budgets it lands in the hundreds of km, which is exactly
 * why the free-space model is useless here and why the comparison below is done
 * in dB and in multiples instead.
 */
static double fspl_range_km(double path_loss_db)
{
	return pow(10.0, (path_loss_db - 32.44 - 20.0 * log10(FREQ_MHZ)) / 20.0);
}

/**
 * Range relative to a reference budget, as a multiple.
 *
 * DELIBERATELY NOT AN ABSOLUTE DISTANCE. A ground-level 433MHz link over a
 * disaster site is limited by Fresnel-zone obstruction and body absorption, not
 * by any log-distance exponent, so every absolute number this file could print
 * would be fiction with three significant figures. What IS trustworthy is the
 * ratio: n dB of extra budget buys 10^(n/(10*path_exponent)) times the distance,
 * whatever the absolute distance turns out to be. Measure the real range once,
 * then use these multipliers.
 */
static double range_multiple(double budget_db, double ref_budget_db, double n)
{
	return pow(10.0, (budget_db - ref_budget_db) / (10.0 * n));
}


struct cfg {
	const char *name;
	unsigned sf;
	double bw_khz;
	unsigned cr;
	unsigned preamble;
};

/* One full poll+reply exchange, worst case (tagged vital), in ms. */
static double slot_ms(const struct cfg *c)
{
	double bw = c->bw_khz * 1000.0;

	return time_on_air_ms(c->sf, bw, c->cr, POLL_BYTES, c->preamble, 1, 0)
	       + time_on_air_ms(c->sf, bw, c->cr, VITAL_MAX_BYTES, c->preamble, 1, 0);
}

/*
 * What the station's receive timeout must cover, measured from the END of its own
 * transmit (which is where sx1278_receive() starts counting).
 *
 * LORA_REPLY_DEADLINE_MS on the node is 150ms: it measures how long a poll may
 * have sat unnoticed between two DIO0 checks and declines to answer if that is
 * exceeded, so 150ms is the worst-case delay before the reply even starts. Add
 * the RX->TX mode switch and the I2C snapshot, then the reply's own airtime.
 *
 * The poll's airtime is NOT part of this: the station has already finished
 * transmitting before the timeout begins.
 */
#define NODE_REPLY_DEADLINE_MS 150u
#define NODE_RX_TX_SWITCH_MS 5u

/** Minimum receive timeout the station needs, worst case (tagged vital). */
static double min_rx_timeout_ms(const struct cfg *c)
{
	double bw = c->bw_khz * 1000.0;

	return NODE_REPLY_DEADLINE_MS + NODE_RX_TX_SWITCH_MS
	       + time_on_air_ms(c->sf, bw, c->cr, VITAL_MAX_BYTES, c->preamble, 1, 0);
}

/**
 * Wall-clock cost of one node's turn: the poll transmit plus the full receive
 * timeout. The station spends the whole timeout on an absent node, so this is
 * also the cost of a node that never answers.
 */
static double slot_cost_ms(const struct cfg *c)
{
	double bw = c->bw_khz * 1000.0;
	double timeout = min_rx_timeout_ms(c);

	if (timeout < (double) LORA_POLL_SLOT_MS) {
		timeout = (double) LORA_POLL_SLOT_MS;
	}
	return time_on_air_ms(c->sf, bw, c->cr, POLL_BYTES, c->preamble, 1, 0)
	       + timeout;
}

static void report(const struct cfg *c, double ref_budget_db)
{
	double bw = c->bw_khz * 1000.0;
	double poll = time_on_air_ms(c->sf, bw, c->cr, POLL_BYTES, c->preamble, 1, 0);
	double vmin = time_on_air_ms(c->sf, bw, c->cr, VITAL_MIN_BYTES, c->preamble, 1, 0);
	double vmax = time_on_air_ms(c->sf, bw, c->cr, VITAL_MAX_BYTES, c->preamble, 1, 0);
	/* The station transmits every poll; each node transmits one reply. */
	double station_duty = (NODES * poll) / (double) PERIOD_MS * 100.0;
	double node_duty = vmax / (double) PERIOD_MS * 100.0;
	double sens = sensitivity_dbm(c->sf, bw);
	/* Node -> station is the weaker direction: the node runs 17dBm (continuous
	 * PA_BOOST) while the station may use 20dBm duty-cycled. */
	double mpl_up = max_path_loss_db(17.0, c->sf, bw);
	double timeout = min_rx_timeout_ms(c);
	double cycle_ms = NODES * slot_cost_ms(c);
	double fits = cycle_ms / PERIOD_MS * 100.0;
	int slot_ok = timeout <= (double) LORA_POLL_SLOT_MS;

	printf("%-20s SF%-2u BW%-4.4g CR4/%u | poll %6.1f  vital %6.1f-%6.1f ms\n",
	       c->name, c->sf, c->bw_khz, c->cr + 4, poll, vmin, vmax);
	printf("%-20s   rx timeout needs %6.1f ms vs SLOT_MS %u %s\n", "", timeout,
	       LORA_POLL_SLOT_MS, slot_ok ? "ok" : "** SLOT TOO SHORT **");
	printf("%-20s   cycle %7.0f ms = %4.0f%% of period %s | duty stn %5.2f%% node %5.2f%%\n",
	       "", cycle_ms, fits, fits > 70.0 ? "** TOO TIGHT **" : "               ",
	       station_duty, node_duty);
	printf("%-20s   sens %6.1f dBm | budget %5.1f dB (%+5.1f dB) | range x%.2f (n=3) x%.2f (n=4)\n\n",
	       "", sens, mpl_up, mpl_up - ref_budget_db,
	       range_multiple(mpl_up, ref_budget_db, 3.0),
	       range_multiple(mpl_up, ref_budget_db, 4.0));
	(void) fspl_range_km;
}

int main(void)
{
	/* Both firmwares agree on these today, by both taking the node library's
	 * newLoRa() defaults. Preamble 8 is also both ends' default. */
	const struct cfg current = { "current (inherited)", 7, 125.0, 1, 8 };

	/* Candidates. Each changes exactly one thing from `current` so the effect
	 * is attributable; the last two combine the two that survive. */
	const struct cfg candidates[] = {
		{ "SF9 (range)", 9, 125.0, 1, 8 },
		{ "SF10 (range)", 10, 125.0, 1, 8 },
		{ "SF12 (max range)", 12, 125.0, 1, 8 },
		{ "BW250 (speed)", 7, 250.0, 1, 8 },
		{ "CR4/8 (robust)", 7, 125.0, 4, 8 },
		{ "SF9 + BW250", 9, 250.0, 1, 8 },
		{ "SF10 + BW250", 10, 250.0, 1, 8 },
		{ "SF11 + BW250", 11, 250.0, 1, 8 },
	};

	printf("TriageBox LoRa budget -- 433MHz, station %.0fdBi / node %.0fdBi, "
	       "%u nodes, %ums cycle\n",
	       G_STATION_DBI, G_NODE_DBI, NODES, PERIOD_MS);
	printf("Budget is the node->station direction (node PA 17dBm, the weaker "
	       "one). Range is a MULTIPLE of the current setting, not km:\n");
	printf("absolute distance at 433MHz on the ground is set by obstruction, "
	       "not by any exponent -- measure once, then scale by these.\n\n");

	double ref = max_path_loss_db(17.0, current.sf, current.bw_khz * 1000.0);

	report(&current, ref);
	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		report(&candidates[i], ref);
	}

	/*
	 * WHERE THE TIME ACTUALLY GOES.
	 *
	 * The table above shows every SF above 7 breaking LORA_POLL_SLOT_MS, which
	 * makes it look like the link is airtime-bound and stuck at SF7. It is not.
	 * At the current setting the reply is 82ms of a 237ms timeout: 155ms -- 65%
	 * of the slot -- is the node's reply deadline plus mode switch, spent waiting
	 * for a node to notice a poll it already received.
	 *
	 * That deadline is a firmware property, not a radio one. It is 150ms because
	 * the node checks DIO0 once per superloop pass and a PN532 scan blocks ~120ms
	 * inside that pass. Cut it and the whole budget reopens.
	 */
	printf("--- where the slot goes, and what shortening the node deadline buys ---\n");
	printf("%-14s %10s %10s %10s %10s\n", "node deadline", "SF7", "SF9",
	       "SF9/BW250", "SF10/BW250");
	static const unsigned deadlines[] = { 150, 100, 75, 50, 30, 20 };
	for (size_t d = 0; d < sizeof(deadlines) / sizeof(deadlines[0]); d++) {
		unsigned dl = deadlines[d];
		struct cfg probe[4] = {
			{ "", 7, 125.0, 1, 8 },
			{ "", 9, 125.0, 1, 8 },
			{ "", 9, 250.0, 1, 8 },
			{ "", 10, 250.0, 1, 8 },
		};

		printf("%10u ms  ", dl);
		for (int i = 0; i < 4; i++) {
			double bw = probe[i].bw_khz * 1000.0;
			double t = dl + NODE_RX_TX_SWITCH_MS
				   + time_on_air_ms(probe[i].sf, bw, probe[i].cr,
						    VITAL_MAX_BYTES, probe[i].preamble,
						    1, 0);
			printf("%7.0f%-3s", t, t <= LORA_POLL_SLOT_MS ? " ok" : " --");
		}
		printf("\n");
	}
	printf("\nSF9/BW250 (+2.0dB, x1.16 range) fits the existing 250ms slot once "
	       "the node deadline is <=70ms.\n");
	printf("SF9/BW125 (+5.0dB, x1.47 range) needs the slot at ~300ms; a 20-node "
	       "cycle is then 6.4s of 15s -- still comfortable.\n\n");

	/*
	 * THE ANSWER DEPENDS ON HOW MANY NODES THERE ARE, and that is the part the
	 * fixed 20-node table above hides.
	 *
	 * Two independent ceilings, and which one binds changes with the node count:
	 *
	 *   slot   the cycle is N slots long, so the per-node budget is PERIOD/N.
	 *          At 2 nodes that is 7.5s and the radio can afford settings that
	 *          are absurd at 20.
	 *   duty   the station transmits N polls per period, and TX duty is a
	 *          regulatory and thermal limit that does NOT relax with fewer
	 *          nodes -- a 5-byte poll at SF12 is 827ms of airtime, so three
	 *          nodes alone put the station over 10%.
	 *
	 * Deploying one firmware to a 2-node bench and a 20-node site therefore
	 * leaves range on the table in the first case and breaks cadence in the
	 * second, which is why this is a table and not a single answer.
	 *
	 * The slot ceiling is 70% of the period, leaving headroom for a cycle that
	 * overruns: xTaskDelayUntil() returns immediately when it does, so the
	 * degradation is a slower cycle rather than a broken one, but a cycle that
	 * routinely overruns makes the MQTT cadence wander.
	 *
	 * ponytail: the 10% duty ceiling is the figure commonly cited for this band
	 * and is NOT confirmed against Indonesian regulation -- see the channel plan
	 * note in docs/lora-air-protocol.md. Treat it as the number to check, not as
	 * the number to trust.
	 */
#define DUTY_CEILING_PCT 10.0

	printf("--- largest usable SF per node count (BW125, CR4/5) ---\n");
	printf("slot ceiling 70%% of a %ums period; TX duty ceiling %.0f%% "
	       "(UNVERIFIED for ID -- see docs)\n\n",
	       PERIOD_MS, DUTY_CEILING_PCT);
	printf("%6s %10s  %7s  %7s  %8s  %6s  %s\n", "nodes", "slot avail", "best SF",
	       "cycle", "TX duty", "range", "limited by");
	static const unsigned counts[] = { 1, 2, 3, 5, 10, 15, 20 };

	for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
		unsigned n = counts[i];
		double budget_ms = PERIOD_MS * 0.7 / n;
		unsigned best = 0;
		const char *bound = "-";
		struct cfg pick = { "", 7, 125.0, 1, 8 };

		for (unsigned sf = 12; sf >= 7; sf--) {
			struct cfg t = { "", sf, 125.0, 1, 8 };
			double duty = n
				      * time_on_air_ms(sf, 125000.0, 1, POLL_BYTES, 8, 1, 0)
				      / (double) PERIOD_MS * 100.0;
			int slot_ok = slot_cost_ms(&t) <= budget_ms;
			int duty_ok = duty <= DUTY_CEILING_PCT;

			if (slot_ok && duty_ok) {
				best = sf;
				pick = t;
				/* Which ceiling stopped us going one step further. */
				struct cfg up = { "", sf + 1, 125.0, 1, 8 };
				double up_duty = n
						 * time_on_air_ms(sf + 1, 125000.0, 1,
								  POLL_BYTES, 8, 1, 0)
						 / (double) PERIOD_MS * 100.0;
				if (sf == 12) {
					bound = "SF12 max";
				} else if (up_duty > DUTY_CEILING_PCT) {
					bound = "TX duty";
				} else if (slot_cost_ms(&up) > budget_ms) {
					bound = "slot";
				}
				break;
			}
		}
		if (best == 0) {
			printf("%6u %8.0f ms  %7s  %7s  %8s  %6s  nothing fits\n", n,
			       budget_ms, "-", "-", "-", "-");
			continue;
		}
		double cycle = n * slot_cost_ms(&pick);
		double duty = n * time_on_air_ms(best, 125000.0, 1, POLL_BYTES, 8, 1, 0)
			      / (double) PERIOD_MS * 100.0;
		double budget = max_path_loss_db(17.0, best, 125000.0);

		printf("%6u %8.0f ms  %7u  %5.1f s  %6.2f%%  x%-5.2f %s\n", n, budget_ms,
		       best, cycle / 1000.0, duty,
		       range_multiple(budget, ref, 3.0), bound);
	}
	printf("\nFewer nodes buy range, but TX duty -- not the slot -- is what binds\n"
	       "below ~10 nodes: a 5-byte poll at SF12 is 827ms of airtime.\n");
	printf("Raising SF also means raising LORA_POLL_SLOT_MS to the 'slot avail'\n"
	       "figure, on BOTH firmwares, or every reply arrives after the timeout.\n\n");

	/*
	 * The concrete change, spelled out, because "raise the spreading factor" is
	 * not actionable and every constant below has a matching one in the node.
	 */
	printf("--- the one-step change, if the 20-node target is real ---\n");
	for (unsigned sf = 7; sf <= 9; sf++) {
		double poll = time_on_air_ms(sf, 125000.0, 1, POLL_BYTES, 8, 1, 0);
		double vmax = time_on_air_ms(sf, 125000.0, 1, VITAL_MAX_BYTES, 8, 1, 0);
		double timeout = NODE_REPLY_DEADLINE_MS + NODE_RX_TX_SWITCH_MS + vmax;
		double slot = ceil(timeout / 10.0) * 10.0;
		double budget = max_path_loss_db(17.0, sf, 125000.0);

		printf("  SF%u  SLOT_MS %4.0f  REPLY_AIRTIME_MS %4.0f  20-node cycle %4.1f s"
		       "  duty %5.2f%%  range x%.2f%s\n",
		       sf, slot, ceil(vmax / 10.0) * 10.0,
		       20.0 * (poll + slot) / 1000.0, 20.0 * poll / PERIOD_MS * 100.0,
		       range_multiple(budget, ref, 3.0),
		       sf == 7 ? "   <- now" : "");
	}
	printf("\n  SF8 is the free one: +2.5dB for 1.8s more cycle and 8.3%% duty.\n");
	printf("  SF9 at 20 nodes is 16.5%% TX duty -- over any plausible ceiling.\n");
	printf("  Constants to change together: LORA_POLL_SLOT_MS (lora_poll.h, both\n");
	printf("  repos), LORA_REPLY_AIRTIME_MS (node main.c, feeds its static assert),\n");
	printf("  and SPREADING_FACTOR in sx1278.c + newLoRa() in the node's LoRa.c.\n\n");

	/*
	 * TWO STATIONS ON ONE CHANNEL, which is the question that gets asked because
	 * changing frequency sounds harder than it is.
	 *
	 * The nodes make it worse than a plain collision problem: lora_poll_for_me()
	 * does not check station_id, so a node answers polls from EITHER station, and
	 * both stations poll radio addresses starting at 1. Station B therefore
	 * publishes station A's node 1 under its own id range -- one patient's vitals
	 * filed against another patient's record, which is the worst failure this
	 * system has.
	 *
	 * Even with that fixed (a station_id check on the node), the channel is still
	 * shared. Station A listens for 250ms after each 31ms poll, and anything
	 * station B or B's nodes transmit inside that window destroys A's reply. The
	 * arithmetic below is the standard unslotted-collision estimate: B's
	 * transmissions are effectively random relative to A's slots, so the expected
	 * number of overlaps is B's transmission count times the vulnerable window,
	 * over the period.
	 */
	printf("--- two stations sharing one channel, %u nodes each (SF7) ---\n", NODES);
	{
		double poll = time_on_air_ms(7, 125000.0, 1, POLL_BYTES, 8, 1, 0);
		double vmax = time_on_air_ms(7, 125000.0, 1, VITAL_MAX_BYTES, 8, 1, 0);
		double timeout = NODE_REPLY_DEADLINE_MS + NODE_RX_TX_SWITCH_MS + vmax;
		/* A's vulnerable window per slot: its own poll plus the listen window. */
		double window = poll + timeout;
		/* B's transmissions per period: N polls from the station, N replies from
		 * its nodes. Each is vulnerable-window + its own length wide. */
		double lambda = NODES * (poll + window) / (double) PERIOD_MS
				+ NODES * (vmax + window) / (double) PERIOD_MS;
		double p_collide = 1.0 - exp(-lambda);

		printf("  A's vulnerable window per slot : %.0f ms (poll %.0f + listen %.0f)\n",
		       window, poll, timeout);
		printf("  B's transmissions per period   : %u polls + %u replies\n", NODES,
		       NODES);
		printf("  expected overlaps per slot     : %.2f\n", lambda);
		printf("  P(slot corrupted)              : %.0f%%\n", p_collide * 100.0);
		printf("\n  So sharing 433MHz between two %u-node stations loses roughly\n",
		       NODES);
		printf("  half the readings, and loses them invisibly -- a collided frame\n");
		printf("  fails CRC and is counted as a miss, identical to a dead node.\n");
		printf("  Two channels (433 and 434 MHz) remove the problem entirely.\n");
		printf("  A different sync word does NOT: it filters after demodulation,\n");
		printf("  so it fixes attribution and not collisions.\n\n");
	}

	/*
	 * The documented airtime figures must keep coming out of these formulas.
	 * They are quoted in docs/lora-air-protocol.md and in lora_vital.h's
	 * airtime rationale, and a slot budget derived from a wrong number is the
	 * failure this whole file is meant to prevent.
	 */
	double bw125 = 125000.0;
	double poll7 = time_on_air_ms(7, bw125, 1, POLL_BYTES, 8, 1, 0);
	double vmin7 = time_on_air_ms(7, bw125, 1, VITAL_MIN_BYTES, 8, 1, 0);
	double vmax7 = time_on_air_ms(7, bw125, 1, VITAL_MAX_BYTES, 8, 1, 0);

	assert(fabs(poll7 - 30.0) < 4.0);  /* documented ~30ms */
	assert(fabs(vmin7 - 51.0) < 4.0);  /* documented ~51ms */
	assert(fabs(vmax7 - 82.0) < 5.0);  /* documented ~82ms */

	/* LDRO must switch itself on exactly where the datasheet says: symbol time
	 * over 16ms, i.e. SF11 and SF12 at BW125, and not before. Both firmwares
	 * derive this independently, so a disagreement here is a silent link
	 * failure. */
	assert((double) (1u << 10) / bw125 * 1000.0 < 16.0);
	assert((double) (1u << 11) / bw125 * 1000.0 > 16.0);

	/* Sensitivity must improve monotonically with SF, and BW250 must cost
	 * exactly 3dB against BW125 (double the noise bandwidth). */
	for (unsigned sf = 7; sf < 12; sf++) {
		assert(sensitivity_dbm(sf + 1, bw125) < sensitivity_dbm(sf, bw125));
	}
	assert(fabs((sensitivity_dbm(7, 250000.0) - sensitivity_dbm(7, bw125)) - 3.01)
	       < 0.05);

	/* A 20-node cycle must fit inside the period with headroom at whatever the
	 * chosen config is, or the poll loop silently stops keeping cadence. */
	assert(NODES * slot_ms(&current) < PERIOD_MS * 0.7);

	printf("lora_budget: all self-checks passed\n");
	return 0;
}

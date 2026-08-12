/* The hero demo: the plugin's loop in miniature.
 *
 * Lives in its own file rather than inline so the site can carry a real
 * Content-Security-Policy — script-src 'self' instead of the 'unsafe-inline'
 * that would make the header decorative — and so the browser can cache it.
 */
/* The hero runs the plugin's loop in miniature: estimate nothing, but displace
 * an accumulation buffer by a per-block motion field and decide, per region,
 * whether it may refresh from the source. Top half refreshes, bottom half does
 * not — which is exactly the difference the Mosh Amount dial controls. */
(function () {
	var canvas = document.getElementById('demo');
	if (!canvas || !canvas.getContext) return;
	var ctx = canvas.getContext('2d', { willReadFrequently: true });
	if (!ctx) return;

	var W = canvas.width, H = canvas.height;
	var BLOCK = 20;
	var COLS = Math.ceil(W / BLOCK), ROWS = Math.ceil(H / BLOCK);
	var FRONT = Math.floor(ROWS * 0.42);   // the row where refreshing stops

	function themeColours() {
		var styles = getComputedStyle(document.documentElement);
		return {
			ground: styles.getPropertyValue('--surface-sunk').trim() || '#eee',
			ink:    styles.getPropertyValue('--ink').trim() || '#111',
			accent: styles.getPropertyValue('--accent').trim() || '#c4006a'
		};
	}

	var colours = themeColours();

	/* Source frame: bars sliding right at a steady rate, plus a disc crossing
	 * the other way, so the block vectors below the front disagree with the
	 * content above it. */
	var source = document.createElement('canvas');
	source.width = W; source.height = H;
	var sctx = source.getContext('2d');

	function drawSource(t) {
		sctx.fillStyle = colours.ground;
		sctx.fillRect(0, 0, W, H);

		sctx.fillStyle = colours.ink;
		sctx.globalAlpha = 0.10;
		for (var i = -2; i < 26; i++) {
			var x = ((i * 60 + t * 46) % (W + 240)) - 120;
			sctx.save();
			sctx.translate(x, 0);
			sctx.transform(1, 0, -0.35, 1, 0, 0);
			sctx.fillRect(0, -40, 26, H + 80);
			sctx.restore();
		}
		sctx.globalAlpha = 1;

		var cx = W * 0.5 + Math.cos(t * 0.55) * W * 0.33;
		var cy = H * 0.44 + Math.sin(t * 0.9) * H * 0.16;
		var grad = sctx.createRadialGradient(cx, cy, 2, cx, cy, 64);
		grad.addColorStop(0, colours.accent);
		grad.addColorStop(1, 'transparent');
		sctx.fillStyle = grad;
		sctx.beginPath();
		sctx.arc(cx, cy, 64, 0, Math.PI * 2);
		sctx.fill();
	}

	/* Per-block motion, in whole pixels. Snapped, because resampling an
	 * accumulation buffer with interpolation every frame turns it to mush —
	 * the same reason Pel Snap defaults to on in the plugin. */
	function vectorFor(col, row, t) {
		var wave = Math.sin(row * 0.55 + t * 0.7) * 0.5 + Math.cos(col * 0.3 - t * 0.5) * 0.5;
		return {
			x: Math.round(3 + wave * 3),
			y: Math.round(Math.sin(col * 0.4 + t * 0.6) * 1.6)
		};
	}

	var time = 0;
	var reduce = window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;

	/* Left to run, the accumulation collapses into mush and stays there. Real
	 * use is a cycle: a burst, then a keyframe that snaps back to clean. This
	 * mirrors what a triggered burst looks like, and keeps the demo watchable. */
	var CYCLE = 7.5, BURST = 5.0;

	function step() {
		time += 1 / 60;
		var phase = time % CYCLE;
		var moshing = phase < BURST;
		drawSource(time);

		/* Displace the accumulated image block by block. drawImage from the
		 * canvas onto itself is the whole motion-compensation step. */
		var carried = ctx.getImageData(0, 0, W, H);
		var scratch = document.createElement('canvas');
		scratch.width = W; scratch.height = H;
		scratch.getContext('2d').putImageData(carried, 0, 0);

		for (var row = 0; row < ROWS; row++) {
			for (var col = 0; col < COLS; col++) {
				var x = col * BLOCK, y = row * BLOCK;
				if (row < FRONT || !moshing) {
					// Above the front, or between bursts: refresh from the source.
					ctx.drawImage(source, x, y, BLOCK, BLOCK, x, y, BLOCK, BLOCK);
				} else {
					// Below it: carry the old pixels along the motion vector.
					var v = vectorFor(col, row, time);
					ctx.drawImage(scratch, x - v.x, y - v.y, BLOCK, BLOCK, x, y, BLOCK, BLOCK);
				}
			}
		}

		// The line where refreshing stops, shown only while a burst is running.
		if (moshing) {
			ctx.fillStyle = colours.accent;
			ctx.fillRect(0, FRONT * BLOCK - 1, W, 1.5);
		}

		if (!reduce) requestAnimationFrame(step);
	}

	// Seed the buffer so the first displaced frame has something to carry.
	drawSource(0);
	ctx.drawImage(source, 0, 0);
	step();

	// The demo samples theme tokens, so it has to be told when they change.
	if (window.matchMedia) {
		var dark = window.matchMedia('(prefers-color-scheme: dark)');
		if (dark.addEventListener) {
			dark.addEventListener('change', function () {
				colours = themeColours();
				drawSource(time);
				ctx.drawImage(source, 0, 0);
			});
		}
	}
})();

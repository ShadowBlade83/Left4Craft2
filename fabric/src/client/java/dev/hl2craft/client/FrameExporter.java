package dev.hl2craft.client;

import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.renderpearl.api.commands.CommandEncoder;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.buffers.GpuBufferSlice;
import dev.hl2craft.HL2Craft;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.VarHandle;
import org.joml.Vector4f;

public final class FrameExporter {
	public static final String NAME = "Local\\MCPassthroughFrame";
	private static final int MAGIC = 0x5450434D; // "MCPT"
	private static final int VERSION = 1;
	private static final int HEADER = 4096;
	private static final int SLOTS = 3;
	private static final int SLOT_DESC = 256;
	private static final int SLOT_DESC_BYTES = 128;
	private static final int MAX_W = 3840;
	private static final int MAX_H = 2160;
	private static final long LAYER_MAX = (long) MAX_W * MAX_H * 4L;
	private static final long STRIDE = LAYER_MAX * 3L;
	private static final int RING = 4;
	private static final long STUCK_NANOS = 1_000_000_000L;
	private static final Vector4f TRANSPARENT = new Vector4f(0.0F, 0.0F, 0.0F, 0.0F);

	private static final ValueLayout.OfInt INT = ValueLayout.JAVA_INT_UNALIGNED;
	private static final ValueLayout.OfLong LONG = ValueLayout.JAVA_LONG_UNALIGNED;
	private static final ValueLayout.OfFloat FLOAT = ValueLayout.JAVA_FLOAT_UNALIGNED;
	private static final ValueLayout.OfDouble DOUBLE = ValueLayout.JAVA_DOUBLE_UNALIGNED;

	private static final float NEAR = 0.05F;
	private static float far = 1024.0F;

	private static SharedMemory shm;
	private static boolean failed;
	private static final Capture[] ring = new Capture[RING];
	private static int ringNext;
	private static int slotNext;
	private static long frameCounter;
	private static long publishCounter;
	private static Capture current;

	private FrameExporter() {
	}

	private static final class Capture {
		GpuBuffer color;
		GpuBuffer depth;
		GpuBuffer overlay;
		int width;
		int height;
		long generation;
		boolean busy;
		boolean ready;
		long busySince;
		HL2Client.Pose pose;
		float far;
		long frame;
		long captureNanos;

		void allocate(int w, int h) {
			this.free();
			long n = (long) w * h * 4L;
			int usage = GpuBuffer.USAGE_MAP_READ | GpuBuffer.USAGE_COPY_DST;
			this.overlay = RenderSystem.getDevice().createBuffer(() -> "hl2craft overlay", usage, n);
			this.width = w;
			this.height = h;
		}

		void free() {
			if (this.overlay != null) {
				this.overlay.close();
				this.overlay = null;
			}
			this.busy = false;
			this.ready = false;
		}
	}

	public static void setFar(float depthFar) {
		far = depthFar;
	}

	private static void shipReadyFrames() {
		Capture newest = null;
		for (Capture c : ring) {
			if (c != null && c.ready && c.frame < frameCounter && (newest == null || c.frame > newest.frame)) {
				newest = c;
			}
		}
		if (newest == null) {
			return;
		}
		publish(newest);
		for (Capture c : ring) {
			if (c != null && c.frame <= newest.frame) {
				c.ready = false;
				c.busy = false;
			}
		}
	}

	private static boolean ensureShm() {
		if (shm != null) {
			return true;
		}
		if (failed) {
			return false;
		}
		try {
			shm = SharedMemory.createOrOpen(NAME, HEADER + STRIDE * SLOTS);
			MemorySegment m = shm.segment;
			m.set(INT, 0, MAGIC);
			m.set(INT, 4, VERSION);
			m.set(INT, 8, HEADER);
			m.set(INT, 12, SLOTS);
			m.set(LONG, 16, STRIDE);
			m.set(INT, 24, MAX_W);
			m.set(INT, 28, MAX_H);
			m.set(INT, 40, -1);
			m.set(INT, 44, (int) ProcessHandle.current().pid());
			HL2Craft.LOG.info("HL2Craft: frame export shm created: {}", NAME);
			return true;
		} catch (Throwable t) {
			failed = true;
			HL2Craft.LOG.error("HL2Craft: frame export disabled (failed to map shm)", t);
			return false;
		}
	}

	public static void captureWorld(RenderTarget target) {
		shipReadyFrames();
		current = null;
		HL2Client.Pose pose = HL2Client.currentPose();
		if (pose == null || !ensureShm()) {
			return;
		}
		int w = target.width;
		int h = target.height;
		if ((long) w * h * 4L > LAYER_MAX) {
			return;
		}

		Capture c = ring[ringNext];
		if (c == null) {
			c = ring[ringNext] = new Capture();
		}
		long now = System.nanoTime();
		if (c.busy && now - c.busySince < STUCK_NANOS) {
			return;
		}
		if (c.width != w || c.height != h || c.overlay == null) {
			HL2Craft.LOG.info("HL2Craft: exporting frame buffer at {}x{}", w, h);
			c.allocate(w, h);
		}

		c.generation++;
		c.busy = true;
		c.ready = false;
		c.busySince = now;
		c.pose = pose;
		c.far = far;
		c.frame = ++frameCounter;
		c.captureNanos = now;

		CommandEncoder encoder = RenderSystem.getDevice().createCommandEncoder();
		encoder.clearColorTexture(target.getColorTexture(), TRANSPARENT);
		current = c;
	}

	public static void captureOverlay(RenderTarget target) {
		Capture c = current;
		current = null;
		if (c == null) {
			return;
		}
		if (target.width != c.width || target.height != c.height) {
			c.busy = false;
			c.ready = false;
			return;
		}

		long gen = c.generation;
		RenderSystem.getDevice().createCommandEncoder().copyTextureToBuffer(target.getColorTexture(), c.overlay, 0L, () -> {
			if (c.generation == gen && c.busy) {
				c.ready = true;
			}
		}, 0);
		ringNext = (ringNext + 1) % RING;
	}

	private static void publish(Capture c) {
		try {
			MemorySegment m = shm.segment;
			int slot = slotNext;
			slotNext = (slotNext + 1) % SLOTS;
			long desc = SLOT_DESC + (long) SLOT_DESC_BYTES * slot;
			long seq = m.get(LONG, desc);
			if ((seq & 1L) != 0L) {
				seq++;
			}
			m.set(LONG, desc, seq + 1L);
			VarHandle.fullFence();

			long base = HEADER + STRIDE * slot;
			long n = (long) c.width * c.height * 4L;
			copy(c.overlay, m, base + 2 * n, n);

			HL2Client.Pose p = c.pose;
			m.set(LONG, desc + 8, c.frame);
			m.set(LONG, desc + 16, 0L);
			m.set(INT, desc + 24, c.width);
			m.set(INT, desc + 28, c.height);
			m.set(FLOAT, desc + 32, NEAR);
			m.set(FLOAT, desc + 36, c.far);
			m.set(FLOAT, desc + 40, p.fov());
			m.set(INT, desc + 44, (RenderSystem.getDevice().getDeviceInfo().isZZeroToOne() ? 1 : 0) | 2 | 4);
			m.set(DOUBLE, desc + 48, p.x());
			m.set(DOUBLE, desc + 56, p.y());
			m.set(DOUBLE, desc + 64, p.z());
			m.set(FLOAT, desc + 72, p.yaw());
			m.set(FLOAT, desc + 76, p.pitch());
			m.set(FLOAT, desc + 80, 0.0F); // roll
			m.set(INT, desc + 84, 1);      // firstPerson
			m.set(LONG, desc + 88, c.captureNanos);
			m.set(LONG, desc + 96, System.nanoTime());
			VarHandle.fullFence();

			m.set(LONG, desc, seq + 2L);
			m.set(INT, 40, slot);
			VarHandle.fullFence();
			m.set(LONG, 32, ++publishCounter);
		} catch (Throwable t) {
			HL2Craft.LOG.warn("HL2Craft: frame export publish failed", t);
		}
	}

	private static void copy(GpuBuffer buffer, MemorySegment dst, long offset, long n) {
		try (GpuBufferSlice.MappedView view = buffer.map(true, false)) {
			MemorySegment.copy(MemorySegment.ofBuffer(view.data()), 0L, dst, offset, n);
		}
	}
}

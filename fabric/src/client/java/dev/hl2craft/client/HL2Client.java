package dev.hl2craft.client;

import dev.hl2craft.HL2Craft;
import dev.hl2craft.link.HL2Link;
import dev.hl2craft.link.Proto;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;

public final class HL2Client {
	public record Pose(double x, double y, double z, float yaw, float pitch, float fov) {}

	private static volatile boolean linked;
	private static Pose currentPose;
	private static long frameCounter;
	private static int appliedViewportW, appliedViewportH;

	private HL2Client() {
	}

	public static boolean linked() {
		return linked;
	}

	public static Pose currentPose() {
		return currentPose;
	}

	public static int appliedViewportW() {
		return appliedViewportW;
	}

	public static int appliedViewportH() {
		return appliedViewportH;
	}

	public static void beginFrame() {
		HL2Link.poll();
		boolean nowLinked = HL2Link.active();
		if (nowLinked != linked) {
			linked = nowLinked;
			HL2Craft.LOG.info("HL2Craft: link state is now {}", linked ? "UP" : "DOWN");
			if (linked) {
				applyLinkedOptions();
				dev.hl2craft.client.render.AvatarExporter.reset();
			} else {
				InputBridge.releaseAll();
			}
		}
		if (!linked) {
			currentPose = null;
			return;
		}

		Minecraft mc = Minecraft.getInstance();
		if (mc.player != null && mc.gui.screen() instanceof net.minecraft.client.gui.screens.DeathScreen) {
			mc.player.respawn();
		}
		InputBridge.drain(mc);

		HL2Link.HostSnapshot host = HL2Link.readHost();
		if (host != null && host.inPlayable()) {
			applyViewportSize(mc, host.width(), host.height());
			currentPose = new Pose(host.x(), host.y(), host.z(), host.yaw(), host.pitch(), host.fov());
			LocalPlayer player = mc.player;
			if (player != null) {
				// Keep player and camera rotation exactly aligned with HL2 camera
				player.setYRot(host.yaw());
				player.setXRot(host.pitch());
				player.setYHeadRot(host.yaw());
				player.setYBodyRot(host.yaw());
				// Overwrite previous tick interpolation values to eliminate rotation blur/smudge
				player.yRotO = host.yaw();
				player.xRotO = host.pitch();
				player.yHeadRotO = host.yaw();
				player.yBodyRotO = host.yaw();

				if (host.hl2PhysicsOverride()) {
					double feetY = host.y() - player.getEyeHeight();
					player.setPos(host.x(), feetY, host.z());
					player.xo = host.x();
					player.yo = feetY;
					player.zo = host.z();
					player.setDeltaMovement(0.0, 0.0, 0.0);
					player.setOnGround(host.onGround());
				} else {
					if (player.isFallFlying()) {
						player.setOnGround(false);
					}
					float pt = mc.getDeltaTracker().getGameTimeDeltaPartialTick(false);
					double lx = net.minecraft.util.Mth.lerp(pt, player.xo, player.getX());
					double ly = net.minecraft.util.Mth.lerp(pt, player.yo, player.getY()) + player.getEyeHeight();
					double lz = net.minecraft.util.Mth.lerp(pt, player.zo, player.getZ());
					currentPose = new Pose(lx, ly, lz, host.yaw(), host.pitch(), host.fov());
				}
			}
		} else {
			currentPose = null;
		}
	}

	private static void applyViewportSize(Minecraft minecraft, int w, int h) {
		if (w <= 0 || h <= 0) {
			return;
		}
		if (w == appliedViewportW && h == appliedViewportH) {
			return;
		}
		appliedViewportW = w;
		appliedViewportH = h;
		minecraft.getWindow().setWindowed(w, h);
		HL2Craft.LOG.info("HL2Craft: sizing window to viewport {}x{}", w, h);
	}

	private static void applyLinkedOptions() {
		Minecraft mc = Minecraft.getInstance();
		mc.options.setCameraType(net.minecraft.client.CameraType.FIRST_PERSON);
		mc.options.pauseOnLostFocus = false;
		mc.options.enableVsync().set(false);
		mc.options.framerateLimit().set(260);
		mc.options.cloudStatus().set(net.minecraft.client.CloudStatus.OFF);
		mc.options.renderDistance().set(6);
		mc.options.simulationDistance().set(6);
		mc.options.autoJump().set(false);
		mc.options.bobView().set(false);
		mc.options.guiScale().set(4);
	}

	public static void afterRender() {
		if (!linked) {
			return;
		}
		Minecraft mc = Minecraft.getInstance();
		LocalPlayer player = mc.player;
		if (player == null) {
			return;
		}

		float partialTick = mc.getDeltaTracker().getGameTimeDeltaPartialTick(false);
		dev.hl2craft.client.render.WorldExporter.frame(mc, partialTick);

		if (mc.options.cloudStatus().get() != net.minecraft.client.CloudStatus.OFF) {
			mc.options.cloudStatus().set(net.minecraft.client.CloudStatus.OFF);
		}
		if (mc.options.framerateLimit().get() < 260) {
			mc.options.framerateLimit().set(260);
		}

		int flags = Proto.MC_IN_WORLD;
		if (mc.gui.screen() != null) {
			flags |= Proto.MC_SCREEN_OPEN;
		}
		if (player.onGround()) {
			flags |= Proto.MC_ON_GROUND;
		}
		if (player.isShiftKeyDown()) {
			flags |= Proto.MC_SNEAKING;
		}
		if (player.isSprinting()) {
			flags |= Proto.MC_SPRINTING;
		}
		if (player.isDeadOrDying()) {
			flags |= Proto.MC_DEAD;
		}
		boolean mcFlight = player.getAbilities().flying || player.isFallFlying();
		if (mcFlight) {
			flags |= Proto.MC_FLYING;
		}
		if (player.isBlocking()) {
			flags |= Proto.MC_BLOCKING;
		}

		if (mc.options.getCameraType() != net.minecraft.client.CameraType.FIRST_PERSON) {
			mc.options.setCameraType(net.minecraft.client.CameraType.FIRST_PERSON);
		}
		int mode = player.isCreative() ? 1 : 0;
		int cameraMode = 0;
		net.minecraft.client.Camera camera = mc.gameRenderer.mainCamera();
		float cameraDistance = 0.0F;

		float supportHeight = 0.0F;
		if (mc.level != null) {
			net.minecraft.core.BlockPos bp = net.minecraft.core.BlockPos.containing(player.getX(), player.getY() - 0.2, player.getZ());
			var state = mc.level.getBlockState(bp);
			if (!state.isAir() && !state.is(net.minecraft.world.level.block.Blocks.BARRIER)) {
				supportHeight = (float) (bp.getY() + 1);
			}
		}

		float sendYaw = camera.isDetached() ? camera.yRot() : player.getYRot();
		float sendPitch = camera.isDetached() ? camera.xRot() : player.getXRot();

		double sendX = net.minecraft.util.Mth.lerp(partialTick, player.xo, player.getX());
		double sendY = net.minecraft.util.Mth.lerp(partialTick, player.yo, player.getY());
		double sendZ = net.minecraft.util.Mth.lerp(partialTick, player.zo, player.getZ());

		int heldLight = 0;
		int heldLightType = 0;
		for (net.minecraft.world.item.ItemStack stack : new net.minecraft.world.item.ItemStack[]{ player.getMainHandItem(), player.getOffhandItem() }) {
			if (stack == null || stack.isEmpty()) continue;
			if (stack.is(net.minecraft.world.item.Items.TORCH)) {
				heldLight = Math.max(heldLight, 14);
				if (heldLightType == 0) heldLightType = 1;
			} else if (stack.is(net.minecraft.world.item.Items.LANTERN) || stack.is(net.minecraft.world.item.Items.CAMPFIRE)
					|| stack.is(net.minecraft.world.item.Items.LAVA_BUCKET) || stack.is(net.minecraft.world.item.Items.GLOWSTONE)
					|| stack.is(net.minecraft.world.item.Items.SEA_LANTERN) || stack.is(net.minecraft.world.item.Items.OCHRE_FROGLIGHT)
					|| stack.is(net.minecraft.world.item.Items.PEARLESCENT_FROGLIGHT) || stack.is(net.minecraft.world.item.Items.VERDANT_FROGLIGHT)) {
				heldLight = Math.max(heldLight, 15);
				if (heldLightType == 0) heldLightType = 1;
			} else if (stack.is(net.minecraft.world.item.Items.SOUL_TORCH)) {
				heldLight = Math.max(heldLight, 10);
				heldLightType = 2;
			} else if (stack.is(net.minecraft.world.item.Items.SOUL_LANTERN) || stack.is(net.minecraft.world.item.Items.SOUL_CAMPFIRE)) {
				heldLight = Math.max(heldLight, 10);
				heldLightType = 2;
			}
		}

		HL2Link.writeMcState(
			flags,
			sendX,
			sendY,
			sendZ,
			sendYaw,
			sendPitch,
			player.getEyeHeight(),
			(float) mc.options.sensitivity().get().doubleValue(),
			0,
			(int) mc.getWindow().getGuiScale(),
			++frameCounter,
			(float) mc.options.fov().get().intValue(),
			mode,
			cameraMode,
			cameraDistance,
			supportHeight,
			player.getHealth(),
			heldLight,
			heldLightType
		);
	}
}

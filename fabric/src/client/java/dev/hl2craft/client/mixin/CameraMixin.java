package dev.hl2craft.client.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.hl2craft.client.FrameExporter;
import dev.hl2craft.client.HL2Client;
import net.minecraft.client.Camera;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.Minecraft;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import org.joml.Quaternionf;
import org.joml.Vector3f;
import org.joml.Vector3fc;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** The host's camera replaces the player's: position, rotation, field of view. */
@Mixin(Camera.class)
abstract class CameraMixin {
	private static final float DEG = (float) (Math.PI / 180.0);
	@Shadow @Final private static Vector3fc FORWARDS;
	@Shadow @Final private static Vector3fc UP;
	@Shadow @Final private static Vector3fc LEFT;
	@Shadow @Final private Vector3f forwards;
	@Shadow @Final private Vector3f up;
	@Shadow @Final private Vector3f left;
	@Shadow @Final private Quaternionf rotation;
	@Shadow private float xRot;
	@Shadow private float yRot;
	@Shadow private boolean detached;
	@Shadow private int matrixPropertiesDirty;
	@Shadow private float depthFar;
	@Shadow @Final private Minecraft minecraft;

	@Shadow
	protected abstract void setPosition(double x, double y, double z);

	@Shadow
	protected abstract void move(float x, float y, float z);

	@Shadow
	protected abstract float getMaxZoom(float startingDistance);

	@Inject(method = "alignWithEntity", at = @At("TAIL"))
	private void hl2craft$hostCamera(final float partialTicks, final CallbackInfo ci) {
		dev.hl2craft.link.HL2Link.HostSnapshot host = dev.hl2craft.link.HL2Link.readHost();
		HL2Client.Pose p = (host != null && host.inPlayable())
			? new HL2Client.Pose(host.x(), host.y(), host.z(), host.yaw(), host.pitch(), host.fov())
			: HL2Client.currentPose();
		if (p == null) {
			return;
		}

		float yaw = p.yaw();
		float pitch = p.pitch();
		if (this.detached && this.minecraft != null && this.minecraft.options.getCameraType().isMirrored()) {
			yaw += 180.0f;
			pitch = -pitch;
		}

		this.xRot = pitch;
		this.yRot = yaw;
		this.rotation.rotationYXZ((float) Math.PI - yaw * DEG, -pitch * DEG, 0.0f);
		FORWARDS.rotate(this.rotation, this.forwards);
		UP.rotate(this.rotation, this.up);
		LEFT.rotate(this.rotation, this.left);
		this.matrixPropertiesDirty |= 3;
		this.setPosition(p.x(), p.y(), p.z());

		if (this.detached) {
			this.move(-this.getMaxZoom(4.0f), 0.0f, 0.0f);
		}
	}

	@WrapOperation(
		method = "getMaxZoom",
		at = @At(
			value = "INVOKE",
			target = "Lnet/minecraft/world/level/Level;clip(Lnet/minecraft/world/level/ClipContext;)Lnet/minecraft/world/phys/BlockHitResult;"
		)
	)
	private BlockHitResult hl2craft$zoomAgainstHL2(
		Level level, ClipContext context, Operation<BlockHitResult> original
	) {
		BlockHitResult vanilla = original.call(level, context);
		BlockHitResult res = dev.hl2craft.world.HL2Clip.refine(context.getFrom(), context.getTo(), vanilla, dev.hl2craft.world.HL2Clip.Use.PROJECTILE);
		if (res.getType() != HitResult.Type.MISS && context.getFrom().distanceTo(res.getLocation()) < 0.8) {
			return vanilla;
		}
		return res;
	}

	@Inject(method = "calculateFov", at = @At("HEAD"), cancellable = true)
	private void hl2craft$hostFov(final float partialTicks, final CallbackInfoReturnable<Float> cir) {
		HL2Client.Pose p = HL2Client.currentPose();
		if (p != null) {
			cir.setReturnValue(p.fov());
		}
	}

	@Inject(method = "update", at = @At("TAIL"))
	private void hl2craft$planes(final DeltaTracker deltaTracker, final CallbackInfo ci) {
		FrameExporter.setFar(this.depthFar);
	}
}

package dev.hl2craft.mixin;

import dev.hl2craft.link.HL2Link;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.GameType;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Player.class)
public abstract class PlayerMixin {
	@Unique
	private static int lastHostGameMode = -1;
	@Unique
	private static int lastTeleportSeq = -1;
	@Unique
	private static int lastCollisionEpoch = -1;

	@Inject(method = "tick", at = @At("HEAD"))
	private void hl2craft$syncServerGameMode(CallbackInfo ci) {
		Player self = (Player) (Object) this;
		if (self.level().isClientSide()) {
			return;
		}

		if (!HL2Link.active()) {
			self.setDeltaMovement(net.minecraft.world.phys.Vec3.ZERO);
			self.fallDistance = 0.0f;
			if (self.getY() < 64.0) {
				self.setPos(self.getX(), 64.0, self.getZ());
			}
			self.xo = self.xOld = self.getX();
			self.yo = self.yOld = self.getY();
			self.zo = self.zOld = self.getZ();
			if (self instanceof net.minecraft.server.level.ServerPlayer sp && sp.connection != null) {
				sp.connection.resetPosition();
			}
			return;
		}

		if (self instanceof net.minecraft.server.level.ServerPlayer sp) {
			HL2Link.HostSnapshot snap = HL2Link.readHost();
			if (snap == null || !snap.inPlayable()) {
				sp.setDeltaMovement(net.minecraft.world.phys.Vec3.ZERO);
				sp.fallDistance = 0.0f;
				if (sp.getY() < 64.0) {
					sp.setPos(sp.getX(), 64.0, sp.getZ());
				}
				sp.xo = sp.xOld = sp.getX();
				sp.yo = sp.yOld = sp.getY();
				sp.zo = sp.zOld = sp.getZ();
				if (sp.connection != null) {
					sp.connection.resetPosition();
				}
				return;
			}
				if (lastCollisionEpoch != -1 && snap.epoch() != lastCollisionEpoch) {
					dev.hl2craft.world.HL2Collision.clearAllPlacedBlocks((net.minecraft.server.level.ServerLevel) sp.level());
					dev.hl2craft.world.HL2Collision.clearAllEntities((net.minecraft.server.level.ServerLevel) sp.level());
				}
				lastCollisionEpoch = snap.epoch();

				var serverLevel = (net.minecraft.server.level.ServerLevel) sp.level();
				if (!serverLevel.getGameRules().get(net.minecraft.world.level.gamerules.GameRules.KEEP_INVENTORY)) {
					serverLevel.getGameRules().set(net.minecraft.world.level.gamerules.GameRules.KEEP_INVENTORY, true, serverLevel.getServer());
				}

				GameType targetMode = (snap.gameMode() == 1) ? GameType.CREATIVE : GameType.SURVIVAL;
				if (sp.gameMode.getGameModeForPlayer() != targetMode) {
					sp.setGameMode(targetMode);
					dev.hl2craft.HL2Craft.LOG.info("HL2Craft: ServerPlayer game mode synchronized to {}", targetMode);
				}

				boolean teleportChanged = (lastTeleportSeq == -1 || snap.teleportSeq() != lastTeleportSeq);
				lastTeleportSeq = snap.teleportSeq();

				double feetY = snap.y() - sp.getEyeHeight();
				dev.hl2craft.world.HL2Collision.ensurePlayerFloorAndStep(sp.getX(), sp.getY(), sp.getZ(), feetY);

				if (teleportChanged || snap.hl2PhysicsOverride()) {
					if (teleportChanged) {
						double dPosSq = sp.position().distanceToSqr(snap.x(), feetY, snap.z());
						if (dPosSq > 1.0) {
							sp.teleportTo(serverLevel, snap.x(), feetY, snap.z(), java.util.Collections.emptySet(), snap.yaw(), snap.pitch(), true);
							sp.setDeltaMovement(net.minecraft.world.phys.Vec3.ZERO);
							if (sp.connection != null) {
								sp.connection.resetPosition();
							}
						}
					} else {
						sp.setPos(snap.x(), feetY, snap.z());
						sp.xo = sp.xOld = snap.x();
						sp.yo = sp.yOld = feetY;
						sp.zo = sp.zOld = snap.z();
						sp.setYRot(snap.yaw());
						sp.setXRot(snap.pitch());
						sp.setDeltaMovement(net.minecraft.world.phys.Vec3.ZERO);
					}
					sp.setOnGround(snap.onGround());
					sp.fallDistance = 0.0f;
				} else {
					sp.setYRot(snap.yaw());
					sp.setXRot(snap.pitch());
					if (sp.isFallFlying() || sp.getAbilities().flying) {
						sp.setOnGround(false);
						sp.fallDistance = 0.0f;
					} else if (snap.onLadder()) {
						sp.fallDistance = 0.0f;
					}
				}
				serverLevel.getChunkSource().move(sp);
		}
	}

	@Inject(method = "isWithinBlockInteractionRange", at = @At("HEAD"), cancellable = true)
	private void hl2craft$allowBlockInteraction(net.minecraft.core.BlockPos pos, double buffer, org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable<Boolean> cir) {
		if (HL2Link.active()) {
			cir.setReturnValue(true);
		}
	}

	@Inject(method = "isWithinEntityInteractionRange(Lnet/minecraft/world/entity/Entity;D)Z", at = @At("HEAD"), cancellable = true)
	private void hl2craft$allowEntityInteraction(net.minecraft.world.entity.Entity entity, double buffer, org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable<Boolean> cir) {
		if (HL2Link.active()) {
			cir.setReturnValue(true);
		}
	}

	@Inject(method = "getDestroySpeed", at = @At("RETURN"), cancellable = true)
	private void hl2craft$cancelMidAirMiningPenalty(net.minecraft.world.level.block.state.BlockState state, org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable<Float> cir) {
		if (HL2Link.active()) {
			Player self = (Player) (Object) this;
			if (!self.onGround()) {
				cir.setReturnValue(cir.getReturnValue() * 5.0f);
			}
		}
	}

	@Inject(method = "hurtServer", at = @At("HEAD"), cancellable = true)
	private void hl2craft$cancelSuffocation(net.minecraft.server.level.ServerLevel level, net.minecraft.world.damagesource.DamageSource source, float amount, org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable<Boolean> cir) {
		if (!HL2Link.active() || lastCollisionEpoch == -1) {
			cir.setReturnValue(false);
			return;
		}
		if (source.is(net.minecraft.world.damagesource.DamageTypes.IN_WALL)
				|| source.is(net.minecraft.world.damagesource.DamageTypes.FALL)
				|| source.is(net.minecraft.world.damagesource.DamageTypes.FELL_OUT_OF_WORLD)) {
			cir.setReturnValue(false);
		}
	}
}

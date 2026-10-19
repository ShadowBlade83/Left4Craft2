package dev.hl2craft.client.mixin;

import dev.hl2craft.client.HL2Client;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.player.Abilities;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(LocalPlayer.class)
abstract class LocalPlayerMixin {
	private static int lastClientTeleportSeq = -1;
	private static int lastClientEpoch = -1;

	@Inject(method = "tick", at = @At("HEAD"), cancellable = true)
	private void hl2craft$followHost(final CallbackInfo ci) {
		LocalPlayer self = (LocalPlayer) (Object) this;
		dev.hl2craft.link.HL2Link.HostSnapshot snap = dev.hl2craft.link.HL2Link.readHost();
		if (snap == null || !snap.inPlayable()) {
			return;
		}

		boolean teleportChanged = (lastClientTeleportSeq == -1 || snap.teleportSeq() != lastClientTeleportSeq || snap.epoch() != lastClientEpoch);
		lastClientTeleportSeq = snap.teleportSeq();
		lastClientEpoch = snap.epoch();

		double hostFeetY = snap.y() - self.getEyeHeight();
		dev.hl2craft.world.HL2Collision.ensurePlayerFloorAndStep(snap.x(), hostFeetY, snap.z(), hostFeetY);

		if (snap.hl2PhysicsOverride()) {
			self.setPos(snap.x(), hostFeetY, snap.z());
			self.xo = self.xOld = snap.x();
			self.yo = self.yOld = hostFeetY;
			self.zo = self.zOld = snap.z();
			self.setYRot(snap.yaw());
			self.setXRot(snap.pitch());
			self.yRotO = snap.yaw();
			self.xRotO = snap.pitch();
			self.yHeadRot = snap.yaw();
			self.yHeadRotO = snap.yaw();
			self.yBodyRot = snap.yaw();
			self.yBodyRotO = snap.yaw();
			self.setDeltaMovement(Vec3.ZERO);
			self.setOnGround(snap.onGround());
			return;
		}

		if (teleportChanged) {
			self.setPos(snap.x(), hostFeetY, snap.z());
			self.xo = self.xOld = snap.x();
			self.yo = self.yOld = hostFeetY;
			self.zo = self.zOld = snap.z();
			self.setDeltaMovement(self.getDeltaMovement().scale(0.5));
		}

		dev.hl2craft.world.HL2Collision.ensurePlayerFloorAndStep(self.getX(), self.getY(), self.getZ(), hostFeetY);
		pushOutOfBlocksAndWalls(self);
		if (self.isFallFlying()) {
			self.setOnGround(false);
		}
	}

	private static void pushOutOfBlocksAndWalls(LocalPlayer self) {
		var level = self.level();
		var box = self.getBoundingBox();
		if (level.noCollision(self, box.deflate(1.0e-4))) {
			return;
		}

		// 1. Try pushing straight UP first (up to 1.05 blocks) — handles pillaring upwards and floor/step overlap
		for (int i = 1; i <= 26; i++) {
			double dy = i * 0.04;
			if (level.noCollision(self, box.move(0.0, dy, 0.0))) {
				self.setPos(self.getX(), self.getY() + dy, self.getZ());
				self.setOnGround(true);
				Vec3 dm = self.getDeltaMovement();
				if (dm.y < 0.0) {
					self.setDeltaMovement(dm.x, 0.0, dm.z);
				}
				return;
			}
		}

		// 2. Try pushing back along previous position vector (e.g. high-speed Elytra flight into a wall)
		double backDx = self.xOld - self.getX();
		double backDy = self.yOld - self.getY();
		double backDz = self.zOld - self.getZ();
		if (backDx * backDx + backDy * backDy + backDz * backDz > 1.0e-4) {
			for (int step = 1; step <= 8; step++) {
				double t = step / 8.0;
				double tx = backDx * t;
				double ty = backDy * t;
				double tz = backDz * t;
				if (level.noCollision(self, box.move(tx, ty, tz))) {
					self.setPos(self.getX() + tx, self.getY() + ty, self.getZ() + tz);
					self.setDeltaMovement(Vec3.ZERO);
					return;
				}
			}
		}

		// 3. Radial 3D push-out to the nearest collision-free space (walls, corners, ceilings)
		double[] yOffsets = { 0.0, 0.25, -0.25, 0.5, -0.5, -0.8 };
		for (int rStep = 1; rStep <= 24; rStep++) {
			double r = rStep * 0.075;
			for (double dy : yOffsets) {
				if (rStep <= 8 && dy == 0.0) {
					// Check pure downward push-out for low ceilings
					if (level.noCollision(self, box.move(0.0, -r, 0.0))) {
						self.setPos(self.getX(), self.getY() - r, self.getZ());
						Vec3 dm = self.getDeltaMovement();
						if (dm.y > 0.0) {
							self.setDeltaMovement(dm.x, 0.0, dm.z);
						}
						return;
					}
				}
				for (int dir = 0; dir < 16; dir++) {
					double angle = dir * (Math.PI / 8.0);
					double dx = Math.cos(angle) * r;
					double dz = Math.sin(angle) * r;
					if (level.noCollision(self, box.move(dx, dy, dz))) {
						self.setPos(self.getX() + dx, self.getY() + dy, self.getZ() + dz);
						Vec3 dm = self.getDeltaMovement();
						double nx = Math.cos(angle);
						double nz = Math.sin(angle);
						double dot = dm.x * nx + dm.z * nz;
						if (dot < 0.0) {
							self.setDeltaMovement(dm.x - dot * nx, dm.y, dm.z - dot * nz);
						}
						return;
					}
				}
			}
		}
	}

	@Inject(method = "tick", at = @At("TAIL"))
	private void hl2craft$afterTickSlopesAndLadders(final CallbackInfo ci) {
		LocalPlayer self = (LocalPlayer) (Object) this;
		dev.hl2craft.link.HL2Link.HostSnapshot snap = dev.hl2craft.link.HL2Link.readHost();
		if (snap == null || !snap.inPlayable() || snap.hl2PhysicsOverride()) {
			return;
		}

		pushOutOfBlocksAndWalls(self);

		if (snap.onLadder() && !self.getAbilities().flying && !self.isFallFlying()) {
			self.fallDistance = 0.0f;
			Vec3 dm = self.getDeltaMovement();
			double vx = Math.clamp(dm.x, -0.15, 0.15);
			double vz = Math.clamp(dm.z, -0.15, 0.15);
			double vy = Math.max(dm.y, -0.15);
			if (self.input != null) {
				if (self.input.keyPresses.forward() || self.input.keyPresses.jump()) {
					vy = 0.22;
				} else if (self.input.keyPresses.shift()) {
					vy = 0.0;
				}
			}
			self.setDeltaMovement(vx, vy, vz);
			return;
		}

		if (!self.getAbilities().flying && !self.isFallFlying() && self.onGround() && self.getDeltaMovement().y <= 0.01) {
			double hostFeetY = snap.y() - self.getEyeHeight();
			double floorTopY = dev.hl2craft.world.HL2Collision.ensurePlayerFloorAndStep(self.getX(), self.getY(), self.getZ(), hostFeetY);
			if (!Double.isNaN(floorTopY) && floorTopY > self.getY() + 0.005 && floorTopY <= self.getY() + 0.68) {
				if (self.level().noCollision(self, self.getBoundingBox().move(0.0, floorTopY - self.getY() + 0.02, 0.0))) {
					self.setPos(self.getX(), floorTopY, self.getZ());
					self.setOnGround(true);
					Vec3 dm = self.getDeltaMovement();
					if (dm.y < 0.0) {
						self.setDeltaMovement(dm.x, 0.0, dm.z);
					}
				}
			} else if (self.horizontalCollision && self.input != null) {
				Vec3 moveVec = self.getLookAngle();
				double len = Math.hypot(moveVec.x, moveVec.z);
				if (len > 1.0e-4 && (self.input.keyPresses.forward() || self.input.keyPresses.backward() || self.input.keyPresses.left() || self.input.keyPresses.right())) {
					double dirX = moveVec.x / len;
					double dirZ = moveVec.z / len;
					double aheadX = self.getX() + dirX * 0.55;
					double aheadZ = self.getZ() + dirZ * 0.55;
					double aheadFloorY = dev.hl2craft.world.HL2Collision.ensurePlayerFloorAndStep(aheadX, self.getY(), aheadZ, hostFeetY);
					if (!Double.isNaN(aheadFloorY) && aheadFloorY > self.getY() + 0.05 && aheadFloorY <= self.getY() + 0.68) {
						double liftY = aheadFloorY - self.getY() + 0.03;
						if (self.level().noCollision(self, self.getBoundingBox().move(dirX * 0.15, liftY, dirZ * 0.15))) {
							self.setPos(self.getX() + dirX * 0.15, aheadFloorY + 0.01, self.getZ() + dirZ * 0.15);
							self.setOnGround(true);
						}
					}
				}
			}
		}
	}
}

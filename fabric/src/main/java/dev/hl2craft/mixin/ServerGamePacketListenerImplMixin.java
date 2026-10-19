package dev.hl2craft.mixin;

import dev.hl2craft.link.HL2Link;
import net.minecraft.network.protocol.game.ServerboundMovePlayerPacket;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.server.network.ServerGamePacketListenerImpl;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(ServerGamePacketListenerImpl.class)
public abstract class ServerGamePacketListenerImplMixin {
	@Shadow public ServerPlayer player;
	@Shadow public abstract void resetPosition();

	@Inject(method = "handleMovePlayer", at = @At("HEAD"), cancellable = true)
	private void hl2craft$handleMovePlayer(ServerboundMovePlayerPacket packet, CallbackInfo ci) {
		if (!this.player.level().getServer().isSameThread()) {
			return;
		}
		if (HL2Link.active()) {
			double prevY = this.player.getY();
			double x = packet.getX(this.player.getX());
			double y = packet.getY(prevY);
			double z = packet.getZ(this.player.getZ());
			double dy = y - prevY;
			float yaw = packet.getYRot(this.player.getYRot());
			float pitch = packet.getXRot(this.player.getXRot());
			this.player.setPos(x, y, z);
			this.player.xo = this.player.xOld = x;
			this.player.yo = this.player.yOld = y;
			this.player.zo = this.player.zOld = z;
			this.player.setYRot(yaw);
			this.player.setXRot(pitch);
			this.player.setDeltaMovement(0.0, dy, 0.0);
			boolean onGround = packet.isOnGround() && !this.player.isFallFlying() && !this.player.getAbilities().flying;
			this.player.setOnGround(onGround);
			if (onGround || this.player.isFallFlying() || this.player.getAbilities().flying) {
				this.player.fallDistance = 0.0f;
			} else if (dy < -0.001) {
				this.player.fallDistance += (float) (-dy);
			}
			((net.minecraft.server.level.ServerLevel) this.player.level()).getChunkSource().move(this.player);
			this.resetPosition();
			ci.cancel();
		}
	}
}

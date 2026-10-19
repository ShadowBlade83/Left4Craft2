package dev.hl2craft.mixin;

import dev.hl2craft.link.HL2Link;
import dev.hl2craft.link.Proto;
import net.minecraft.core.BlockPos;
import net.minecraft.world.InteractionResult;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.item.BlockItem;
import net.minecraft.world.item.context.BlockPlaceContext;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(BlockItem.class)
public abstract class BlockItemMixin {
	@Inject(method = "place", at = @At("HEAD"), cancellable = true)
	private void hl2craft$preventPlaceInsideHl2Player(BlockPlaceContext context, CallbackInfoReturnable<InteractionResult> cir) {
		if (!HL2Link.active() || context == null) {
			return;
		}
		BlockPos pos = context.getClickedPos();
		BlockItem self = (BlockItem) (Object) this;
		BlockState defaultState = self.getBlock().defaultBlockState();
		if (defaultState.getCollisionShape(context.getLevel(), pos).isEmpty()) {
			return;
		}

		// Block placement only if the block would overlap the player's upper body (chest/head).
		// If it overlaps the lower legs/feet while jumping (pillaring up), allow placement and push the player up onto the block!
		final double halfW = 0.29;
		final double bx0 = pos.getX();
		final double bx1 = bx0 + 1.0;
		final double by0 = pos.getY();
		final double by1 = by0 + 1.0;
		final double bz0 = pos.getZ();
		final double bz1 = bz0 + 1.0;

		Player player = context.getPlayer();
		if (player != null) {
			double px = player.getX();
			double py = player.getY();
			double pz = player.getZ();
			double pHeight = player.getDimensions(player.getPose()).height();
			if (px + halfW > bx0 && px - halfW < bx1 &&
				pz + halfW > bz0 && pz - halfW < bz1) {
				// Block is at head/chest level -> prevent placing inside upper body
				if (py + pHeight > by0 + 0.02 && py + 0.55 < by1) {
					cir.setReturnValue(InteractionResult.FAIL);
					return;
				}
			}
		}
	}

	@Inject(method = "canPlace", at = @At("HEAD"), cancellable = true)
	private void hl2craft$allowPillarPlaceUnderFeet(BlockPlaceContext context, BlockState state, CallbackInfoReturnable<Boolean> cir) {
		if (!HL2Link.active() || context == null) {
			return;
		}
		Player player = context.getPlayer();
		if (player == null) {
			return;
		}
		BlockPos pos = context.getClickedPos();
		double halfW = 0.30;
		double px = player.getX();
		double py = player.getY();
		double pz = player.getZ();
		double bx0 = pos.getX(), bx1 = bx0 + 1.0;
		double by0 = pos.getY(), by1 = by0 + 1.0;
		double bz0 = pos.getZ(), bz1 = bz0 + 1.0;
		if (px + halfW > bx0 && px - halfW < bx1 &&
			pz + halfW > bz0 && pz - halfW < bz1 &&
			py + 0.55 >= by1 && py < by1 + 0.05) {
			double liftY = (by1 + 0.001) - py;
			if (liftY > 0.0 && liftY <= 0.55 && player.level().noCollision(player, player.getBoundingBox().move(0.0, liftY, 0.0))) {
				cir.setReturnValue(true);
			}
		}
	}

	@Inject(method = "place", at = @At("RETURN"))
	private void hl2craft$onBlockPlaced(BlockPlaceContext context, CallbackInfoReturnable<InteractionResult> cir) {
		if (HL2Link.active() && cir.getReturnValue().consumesAction()) {
			BlockPos pos = context.getClickedPos();
			BlockState placed = context.getLevel().getBlockState(pos);
			int light = placed.getLightEmission();
			int lightType = 0;
			if (light > 0) {
				lightType = (placed.is(net.minecraft.world.level.block.Blocks.SOUL_TORCH)
					|| placed.is(net.minecraft.world.level.block.Blocks.SOUL_WALL_TORCH)
					|| placed.is(net.minecraft.world.level.block.Blocks.SOUL_LANTERN)
					|| placed.is(net.minecraft.world.level.block.Blocks.SOUL_CAMPFIRE)
					|| placed.is(net.minecraft.world.level.block.Blocks.SOUL_FIRE)) ? 2 : 1;
			}
			boolean hasCollision = !placed.getCollisionShape(context.getLevel(), pos).isEmpty();
			if (hasCollision || light > 0) {
				if (hasCollision) {
					dev.hl2craft.world.HL2Collision.trackPlacedBlock(pos);
				}
				HL2Link.pushEvent(Proto.EV_BLOCK_PLACED, 0, pos.getX(), pos.getY(), pos.getZ(), (float) light, lightType, hasCollision ? 1 : 0);

				// If player's feet/legs overlap the newly placed solid block (e.g. pillaring upwards), push player onto top of block
				Player player = context.getPlayer();
				if (player != null && hasCollision) {
					double halfW = 0.30;
					double px = player.getX();
					double py = player.getY();
					double pz = player.getZ();
					double bx0 = pos.getX(), bx1 = bx0 + 1.0;
					double by0 = pos.getY(), by1 = by0 + 1.0;
					double bz0 = pos.getZ(), bz1 = bz0 + 1.0;
					if (px + halfW > bx0 && px - halfW < bx1 &&
						pz + halfW > bz0 && pz - halfW < bz1 &&
						py < by1 && py + 1.8 > by0) {
						double targetY = by1 + 0.001;
						if (targetY - py <= 1.05 && player.level().noCollision(player, player.getBoundingBox().move(0.0, targetY - py, 0.0))) {
							player.setPos(px, targetY, pz);
							player.setOnGround(true);
							var dm = player.getDeltaMovement();
							if (dm.y < 0.0) {
								player.setDeltaMovement(dm.x, 0.0, dm.z);
							}
						}
					}
				}
			}
		}
	}
}

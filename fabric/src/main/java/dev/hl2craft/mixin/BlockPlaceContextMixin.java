package dev.hl2craft.mixin;

import dev.hl2craft.world.HL2Clip;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.context.BlockPlaceContext;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.BlockHitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(BlockPlaceContext.class)
public abstract class BlockPlaceContextMixin {
	@Shadow
	protected boolean replaceClicked;

	@Inject(
		method = "<init>(Lnet/minecraft/world/level/Level;Lnet/minecraft/world/entity/player/Player;Lnet/minecraft/world/InteractionHand;Lnet/minecraft/world/item/ItemStack;Lnet/minecraft/world/phys/BlockHitResult;)V",
		at = @At("RETURN")
	)
	private void hl2craft$fixPlacementOnHL2(Level level, Player player, InteractionHand hand, ItemStack itemStack, BlockHitResult hit, CallbackInfo ci) {
		// HL2Clip.refine (Use.PICK) already resolves hit.getBlockPos() to the target air placement cell (like SkyCraft),
		// so vanilla replaceClicked logic works identically on both client and server without desync.
	}
}

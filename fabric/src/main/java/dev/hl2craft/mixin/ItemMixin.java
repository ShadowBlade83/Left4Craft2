package dev.hl2craft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.hl2craft.world.HL2Clip;
import net.minecraft.world.item.Item;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.BlockHitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

@Mixin(Item.class)
public abstract class ItemMixin {
	@WrapOperation(
		method = "getPlayerPOVHitResult",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;clip(Lnet/minecraft/world/level/ClipContext;)Lnet/minecraft/world/phys/BlockHitResult;")
	)
	private static BlockHitResult hl2craft$povHL2(Level level, ClipContext context, Operation<BlockHitResult> original) {
		return HL2Clip.refine(context.getFrom(), context.getTo(), original.call(level, context));
	}
}

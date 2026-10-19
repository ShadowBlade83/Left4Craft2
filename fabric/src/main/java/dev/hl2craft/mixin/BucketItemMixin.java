package dev.hl2craft.mixin;

import dev.hl2craft.link.HL2Link;
import dev.hl2craft.link.Proto;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.item.BucketItem;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.material.Fluid;
import net.minecraft.world.level.material.Fluids;
import net.minecraft.world.phys.BlockHitResult;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(BucketItem.class)
public abstract class BucketItemMixin {
	@Shadow
	@Final
	private Fluid content;

	@Inject(method = "emptyContents", at = @At("RETURN"))
	private void hl2craft$trackLavaPlaced(LivingEntity entity, Level level, BlockPos pos, BlockHitResult hitResult, CallbackInfoReturnable<Boolean> cir) {
		if (HL2Link.active() && cir.getReturnValue() && this.content == Fluids.LAVA) {
			dev.hl2craft.world.HL2Collision.trackPlacedBlock(pos);
			HL2Link.pushEvent(Proto.EV_BLOCK_PLACED, 0, pos.getX(), pos.getY(), pos.getZ(), 15.0f, 1, 0);
		}
	}
}

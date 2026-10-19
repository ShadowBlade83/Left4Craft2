package dev.hl2craft.mixin;

import dev.hl2craft.link.HL2Link;
import dev.hl2craft.link.Proto;
import dev.hl2craft.world.HL2Collision;
import net.minecraft.advancements.triggers.CriteriaTriggers;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.sounds.SoundEvents;
import net.minecraft.sounds.SoundSource;
import net.minecraft.world.InteractionResult;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.item.FlintAndSteelItem;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.context.UseOnContext;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.BaseFireBlock;
import net.minecraft.world.level.gameevent.GameEvent;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(FlintAndSteelItem.class)
public abstract class FlintAndSteelItemMixin {
	@Inject(method = "useOn", at = @At("HEAD"), cancellable = true)
	private void hl2craft$igniteHL2Floor(UseOnContext context, CallbackInfoReturnable<InteractionResult> cir) {
		if (!HL2Link.active() || context == null) {
			return;
		}
		Level level = context.getLevel();
		BlockPos clickedPos = context.getClickedPos();
		BlockPos firePos = clickedPos;
		if (!level.getBlockState(firePos).isAir()) {
			firePos = clickedPos.relative(context.getClickedFace());
		}
		if (level.getBlockState(firePos).isAir() && HL2Collision.hasGeometry(firePos.below())) {
			Player player = context.getPlayer();
			level.playSound(player, firePos, SoundEvents.FLINTANDSTEEL_USE, SoundSource.BLOCKS, 1.0F, level.getRandom().nextFloat() * 0.4F + 0.8F);
			level.setBlock(firePos, BaseFireBlock.getState(level, firePos), 11);
			level.gameEvent(player, GameEvent.BLOCK_PLACE, firePos);
			HL2Link.pushEvent(Proto.EV_BLOCK_PLACED, 0, firePos.getX(), firePos.getY(), firePos.getZ(), 15.0f, 1, 0);
			ItemStack stack = context.getItemInHand();
			if (player instanceof ServerPlayer sp) {
				CriteriaTriggers.PLACED_BLOCK.trigger(sp, firePos, stack);
				stack.hurtAndBreak(1, player, context.getHand().asEquipmentSlot());
			}
			cir.setReturnValue(InteractionResult.SUCCESS);
		}
	}
}

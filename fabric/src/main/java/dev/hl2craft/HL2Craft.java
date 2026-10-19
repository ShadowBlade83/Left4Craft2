package dev.hl2craft;

import dev.hl2craft.combat.HL2Combat;
import dev.hl2craft.world.HL2Collision;
import net.fabricmc.api.ModInitializer;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents;
import net.fabricmc.fabric.api.networking.v1.ServerPlayConnectionEvents;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.EquipmentSlot;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.level.gamerules.GameRules;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

public final class HL2Craft implements ModInitializer {
	public static final String MOD_ID = "hl2craft";
	public static final String WORLD_NAME = "HL2World";
	public static final Logger LOG = LoggerFactory.getLogger(MOD_ID);

	@Override
	public void onInitialize() {
		LOG.info("HL2Craft initializing...");
		HL2Combat.init();
		HL2Collision.startConsumer();
		net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents.END_SERVER_TICK.register(HL2Collision::serverTick);

		ServerLifecycleEvents.SERVER_STARTED.register(HL2Craft::configureServer);
		ServerPlayConnectionEvents.JOIN.register((handler, sender, server) -> {
			giveStarterKit(handler.getPlayer());
		});
	}

	private static void configureServer(MinecraftServer server) {
		GameRules rules = server.getGameRules();
		rules.set(GameRules.ADVANCE_TIME, false, server);
		rules.set(GameRules.ADVANCE_WEATHER, false, server);
		rules.set(GameRules.PLAYER_MOVEMENT_CHECK, false, server);
		rules.set(GameRules.KEEP_INVENTORY, true, server);
		rules.set(GameRules.IMMEDIATE_RESPAWN, true, server);
		rules.set(GameRules.SHOW_ADVANCEMENT_MESSAGES, false, server);
		server.getCommands().performPrefixedCommand(server.createCommandSourceStack().withSuppressedOutput(), "time set noon");
		server.getCommands().performPrefixedCommand(server.createCommandSourceStack().withSuppressedOutput(), "weather clear");
		LOG.info("HL2Craft: server world rules configured cleanly");
	}

	private static void giveStarterKit(ServerPlayer player) {
		var inv = player.getInventory();
		if (!inv.isEmpty()) {
			boolean hasAxe = false;
			for (int i = 0; i < inv.getContainerSize(); i++) {
				if (inv.getItem(i).is(Items.DIAMOND_AXE)) {
					hasAxe = true;
					break;
				}
			}
			if (!hasAxe) {
				inv.add(new ItemStack(Items.DIAMOND_AXE));
			}
			return;
		}
		inv.add(new ItemStack(Items.DIAMOND_SWORD));
		inv.add(new ItemStack(Items.DIAMOND_AXE));
		inv.add(new ItemStack(Items.DIAMOND_PICKAXE));
		inv.add(new ItemStack(Items.BOW));
		inv.add(new ItemStack(Items.ARROW, 64));
		inv.add(new ItemStack(Items.COOKED_BEEF, 32));
		inv.add(new ItemStack(Items.OAK_PLANKS, 64));
		inv.add(new ItemStack(Items.COBBLESTONE, 64));
		inv.add(new ItemStack(Items.TORCH, 32));
		inv.add(new ItemStack(Items.TNT, 32));
		inv.add(new ItemStack(Items.FLINT_AND_STEEL));
		inv.add(new ItemStack(Items.ENDER_PEARL, 16));
		player.setItemSlot(EquipmentSlot.OFFHAND, new ItemStack(Items.SHIELD));
		LOG.info("HL2Craft: gave starter kit to player {}", player.getName().getString());
	}
}

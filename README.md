# Sample-PersistenceLab
This is a sample project to accompany the Unreal Fest Chicago 2026 talk: The Player was Here - Persisting World State and Actor LOD Systems. In this example, basic maps demonstrate common gameplay saving needs for actors, instanced actors and Mass entities. Those maps can be traveled in-between and can be saved and restored at any point. Map placed actors get their properties restored. Spawned actors and Mass entities get respawned with their state from the previous session. A SaveGame class, world subsystem and game instance subsystem together coordinate storing and reapplying state for the persistent levels and (world partition) streaming levels.

# Instructions
The project is compatible with UE 5.8.0. Launch MAP_MainMenu in PIE. Press Tab key at any moment to open the pause menu that gives access to quick saving and loading. Explore the different maps for interactable actors, spawned pickups, State Tree NPCs, destructible Instanced Actors and hydrated/dehydrated Mass entities.

The PersistenceUtils plugin in this project is meant to be serve as an example, and to be portable basis and modifiable by you. I may update it over time though, so check back in here. This code has not been battle tested in a shipped title. Feedback is welcome!

# Featured UE plugins
Level Streaming Persistence Plugin (experimental) - available since UE 5.3 with new features incoming in UE 5.8.

Instanced Actors Plugin (experimental) - available since UE 5.4 and used in shipped Epic titles.

Mass Entity framework - production ready as of UE 5.2, though Mass gameplay and AI plugins remain experimental.

# Useful resources
Resources to get you started with systems demonstrated in the talk.

## The Player was Here: Persisting World State and Actor LOD Systems
This Unreal Fest presentation is the reason for this sample project.

Live presentation: happened, expecting recordings in July or August 2026.

Text version: https://dev.epicgames.com/community/learning/tutorials/1mj4/unreal-engine-saving-world-state-with-actors-instanced-actors-and-mass

## Your First 60 Minutes with Instanced Actors
https://dev.epicgames.com/community/learning/tutorials/eGYq/unreal-engine-your-first-60-minutes-with-instanced-actors

## Instanced Actors Customization and Replication
https://dev.epicgames.com/community/learning/tutorials/dOW5/unreal-engine-instanced-actors-customization-and-replication

## Your First 60 Minutes with Mass by James Keeling
https://dev.epicgames.com/community/learning/tutorials/6vG6/unreal-engine-your-first-60-minutes-with-mass

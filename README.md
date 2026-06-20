# Sample-PersistenceLab
This is a sample project to accompany an upcoming Unreal Fest Chicago 2026 talk: The Player was Here - Persisting World State and Actor LOD Systems. In this example, basic maps demonstrate common gameplay saving needs for actors, instanced actors and Mass entities. Those maps can be traveled in-between and can be saved and restored at any point. Map placed actors get their properties restored. Spawned actors and Mass entities get respawned with their state from the previous session. A SaveGame class, world subsystem and game instance subsystem together coordinate storing and reapplying state for the persistent levels and (world partition) streaming levels.

# Instructions
The project is compatible with UE 5.8.0+ (not 5.8 preview 1). Either compile the 5.8.0 release candidate from UE GitHub yourself or wait for its release. Launch MAP_MainMenu in PIE. Press Tab key at any moment to open the pause menu that gives access to quick saving and loading. Explore the different maps for interactable actors, spawned pickups, State Tree NPCs, destructible Instanced Actors and hydrated/dehydrated Mass entities.

The PersistenceUtils plugin in this project is meant to be serve as an example, and to be portable basis and modifiable by you. I may update it over time though, so check back in here. This code has not been battle tested in a shipped title. Feedback is welcome!

# Featured UE plugins
Level Streaming Persistence Plugin (experimental) - available since UE 5.3 with new features incoming in UE 5.8.

Instanced Actors Plugin (experimental) - available since UE 5.4 and used in shipped Epic titles.

Mass Entity framework - production ready as of UE 5.2, though Mass gameplay and AI plugins remain experimental.

# Useful resources
Resources to get you started with systems demonstrated in the talk.

## Incoming: The Player was Here - presentation and text version
Presentation at Unreal Day 2026 in Belgrade, Serbia and Unreal Fest Chicago 2026. 

Text version of the talk will be available right before Unreal Fest (June 17, 2026).

Recording of Unreal Fest talk will likely be uploaded later in 2026.

## Incoming: Your First 60 Minutes with Instanced Actors
I'm preparing a text tutorial for Instanced Actors. Check back late June 2026.

## Your First 60 Minutes with Mass by James Keeling
https://dev.epicgames.com/community/learning/tutorials/6vG6/unreal-engine-your-first-60-minutes-with-mass

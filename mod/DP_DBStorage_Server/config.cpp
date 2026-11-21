// DP_DBStorage_Server — PBO-часть graft-мода DP_DBStorage.
// Своих скриптов у мода нет: PBO несёт только объявление натива DbStatus(), которое
// печатает сборка плагина. Без ядра GRAFT объявление скрыто под #ifdef.

class CfgPatches
{
	class DP_DBStorage_Server
	{
		units[] = {};
		weapons[] = {};
		requiredVersion = 0.1;
		requiredAddons[] = { "DZ_Data" };
	};
};

class CfgMods
{
	class DP_DBStorage_Server
	{
		dir = "DP_DBStorage_Server";
		picture = "";
		action = "";
		hideName = 1;
		hidePicture = 1;
		name = "DP DBStorage (Server)";
		credits = "DarkProject";
		author = "DarkProject";
		authorID = "0";
		version = "2.0.0";
		extra = 0;
		type = "servermod";                // только сервер, клиентам не уходит

		dependencies[] = { "Core", "World", "Game", "Mission" };

		class defs
		{
			class gameScriptModule
			{
				value = "";
				files[] = { "DP_DBStorage_Server/scripts/3_Game" };
			};
		};
	};
};

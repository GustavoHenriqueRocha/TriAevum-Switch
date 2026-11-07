#pragma once

// Actor names by actor id for the actor cost profile (config/actor_profile):
// the ids of decomp_support/include/oot3d/actor_object_semantics.h
// (Oot3dActorId, 471 entries from the N64 decomp), each ACTOR_X_Y spelled as
// the decomp's overlay name X_Y with each word capitalized (ACTOR_EN_WF ->
// "En_Wf", ACTOR_PLAYER -> "Player"); ACTOR_UNSET_n are nullptr.
// oot3d_native_perf_diagnostics_tests checks every entry against that header.
// The names are the overlay names of the zeldaret/oot decompilation's actor
// table (https://github.com/zeldaret/oot, include/tables/actor_table.h; no
// license), used as identifiers only.

#include <array>
#include <cstddef>
#include <cstdint>

namespace Oot3dNativeGame {

inline constexpr std::array<const char *, 471> kNativeActorNames{
    "Player", nullptr, "En_Test", nullptr, "En_Girla", nullptr, nullptr, "En_Part", "En_Light",
    "En_Door", "En_Box", "Bg_Dy_Yoseizo", "Bg_Hidan_Firewall", "En_Poh", "En_Okuta",
    "Bg_Ydan_Sp", "En_Bom", "En_Wallmas", "En_Dodongo", "En_Firefly", "En_Horse", "En_Item00",
    "En_Arrow", nullptr, "En_Elf", "En_Niw", nullptr, "En_Tite", "En_Reeba", "En_Peehat",
    "En_Butte", nullptr, "En_Insect", "En_Fish", nullptr, "En_Holl", "En_Scene_Change", "En_Zf",
    "En_Hata", "Boss_Dodongo", "Boss_Goma", "En_Zl1", "En_Viewer", "En_Goma", "Bg_Pushbox",
    "En_Bubble", "Door_Shutter", "En_Dodojr", "En_Bdfire", nullptr, "En_Boom", "En_Torch2",
    "En_Bili", "En_Tp", nullptr, "En_St", "En_Bw", "En_A_Obj", "En_Eiyer", "En_River_Sound",
    "En_Horse_Normal", "En_Ossan", "Bg_Treemouth", "Bg_Dodoago", "Bg_Hidan_Dalm",
    "Bg_Hidan_Hrock", "En_Horse_Ganon", "Bg_Hidan_Rock", "Bg_Hidan_Rsekizou",
    "Bg_Hidan_Sekizou", "Bg_Hidan_Sima", "Bg_Hidan_Syoku", "En_Xc", "Bg_Hidan_Curtain",
    "Bg_Spot00_Hanebasi", "En_Mb", "En_Bombf", "En_Zl2", "Bg_Hidan_Fslift", "En_Oe2",
    "Bg_Ydan_Hasi", "Bg_Ydan_Maruta", "Boss_Ganondrof", nullptr, "En_Am", "En_Dekubaba",
    "En_M_Fire1", "En_M_Thunder", "Bg_Ddan_Jd", "Bg_Breakwall", "En_Jj", "En_Horse_Zelda",
    "Bg_Ddan_Kd", "Door_Warp1", "Obj_Syokudai", "Item_B_Heart", "En_Dekunuts",
    "Bg_Menkuri_Kaiten", "Bg_Menkuri_Eye", "En_Vali", "Bg_Mizu_Movebg", "Bg_Mizu_Water",
    "Arms_Hook", "En_Fhg", "Bg_Mori_Hineri", "En_Bb", "Bg_Toki_Hikari", "En_Yukabyun",
    "Bg_Toki_Swd", "En_Fhg_Fire", "Bg_Mjin", "Bg_Hidan_Kousi", "Door_Toki", "Bg_Hidan_Hamstep",
    "En_Bird", nullptr, nullptr, nullptr, nullptr, "En_Wood02", nullptr, nullptr, nullptr,
    nullptr, "En_Lightbox", "En_Pu_Box", nullptr, nullptr, "En_Trap", "En_Arow_Trap", "En_Vase",
    nullptr, "En_Ta", "En_Tk", "Bg_Mori_Bigst", "Bg_Mori_Elevator", "Bg_Mori_Kaitenkabe",
    "Bg_Mori_Rakkatenjo", "En_Vm", "Demo_Effect", "Demo_Kankyo", "Bg_Hidan_Fwbig",
    "En_Floormas", "En_Heishi1", "En_Rd", "En_Po_Sisters", "Bg_Heavy_Block", "Bg_Po_Event",
    "Obj_Mure", "En_Sw", "Boss_Fd", "Object_Kankyo", "En_Du", "En_Fd", "En_Horse_Link_Child",
    "Door_Ana", "Bg_Spot02_Objects", "Bg_Haka", "Magic_Wind", "Magic_Fire", nullptr, "En_Ru1",
    "Boss_Fd2", "En_Fd_Fire", "En_Dh", "En_Dha", "En_Rl", "En_Encount1", "Demo_Du", "Demo_Im",
    "Demo_Tre_Lgt", "En_Fw", "Bg_Vb_Sima", "En_Vb_Ball", "Bg_Haka_Megane", "Bg_Haka_Meganebg",
    "Bg_Haka_Ship", "Bg_Haka_Sgami", nullptr, "En_Heishi2", "En_Encount2", "En_Fire_Rock",
    "En_Brob", "Mir_Ray", "Bg_Spot09_Obj", "Bg_Spot18_Obj", "Boss_Va", "Bg_Haka_Tubo",
    "Bg_Haka_Trap", "Bg_Haka_Huta", "Bg_Haka_Zou", "Bg_Spot17_Funen", "En_Syateki_Itm",
    "En_Syateki_Man", "En_Tana", "En_Nb", "Boss_Mo", "En_Sb", "En_Bigokuta", "En_Karebaba",
    "Bg_Bdan_Objects", "Demo_Sa", "Demo_Go", "En_In", "En_Tr", "Bg_Spot16_Bombstone", nullptr,
    "Bg_Hidan_Kowarerukabe", "Bg_Bombwall", "Bg_Spot08_Iceblock", "En_Ru2", "Obj_Dekujr",
    "Bg_Mizu_Uzu", "Bg_Spot06_Objects", "Bg_Ice_Objects", "Bg_Haka_Water", nullptr, "En_Ma2",
    "En_Bom_Chu", "En_Horse_Game_Check", "Boss_Tw", "En_Rr", "En_Ba", "En_Bx", "En_Anubice",
    "En_Anubice_Fire", "Bg_Mori_Hashigo", "Bg_Mori_Hashira4", "Bg_Mori_Idomizu",
    "Bg_Spot16_Doughnut", "Bg_Bdan_Switch", "En_Ma1", "Boss_Ganon", "Boss_Sst", nullptr,
    nullptr, "En_Ny", "En_Fr", "Item_Shield", "Bg_Ice_Shelter", "En_Ice_Hono", "Item_Ocarina",
    nullptr, nullptr, "Magic_Dark", "Demo_6k", "En_Anubice_Tag", "Bg_Haka_Gate",
    "Bg_Spot15_Saku", "Bg_Jya_Goroiwa", "Bg_Jya_Zurerukabe", nullptr, "Bg_Jya_Cobra",
    "Bg_Jya_Kanaami", "Fishing", "Obj_Oshihiki", "Bg_Gate_Shutter", "Eff_Dust",
    "Bg_Spot01_Fusya", "Bg_Spot01_Idohashira", "Bg_Spot01_Idomizu", "Bg_Po_Syokudai",
    "Bg_Ganon_Otyuka", "Bg_Spot15_Rrbox", "Bg_Umajump", nullptr, "Arrow_Fire", "Arrow_Ice",
    "Arrow_Light", nullptr, nullptr, "Item_Etcetera", "Obj_Kibako", "Obj_Tsubo",
    "En_Wonder_Item", "En_Ik", "Demo_Ik", "En_Skj", "En_Skjneedle", "En_G_Switch", "Demo_Ext",
    "Demo_Shd", "En_Dns", "Elf_Msg", "En_Honotrap", "En_Tubo_Trap", "Obj_Ice_Poly",
    "Bg_Spot03_Taki", "Bg_Spot07_Taki", "En_Fz", "En_Po_Relay", "Bg_Relay_Objects",
    "En_Diving_Game", "En_Kusa", "Obj_Bean", "Obj_Bombiwa", nullptr, nullptr, "Obj_Switch",
    "Obj_Elevator", "Obj_Lift", "Obj_Hsblock", "En_Okarina_Tag", "En_Yabusame_Mark",
    "En_Goroiwa", "En_Ex_Ruppy", "En_Toryo", "En_Daiku", nullptr, "En_Nwc", "En_Blkobj",
    "Item_Inbox", "En_Ge1", "Obj_Blockstop", "En_Sda", "En_Clear_Tag", "En_Niw_Lady", "En_Gm",
    "En_Ms", "En_Hs", "Bg_Ingate", "En_Kanban", "En_Heishi3", "En_Syateki_Niw", "En_Attack_Niw",
    "Bg_Spot01_Idosoko", "En_Sa", "En_Wonder_Talk", "Bg_Gjyo_Bridge", "En_Ds", "En_Mk",
    "En_Bom_Bowl_Man", "En_Bom_Bowl_Pit", "En_Owl", "En_Ishi", "Obj_Hana", "Obj_Lightswitch",
    "Obj_Mure2", "En_Go", "En_Fu", nullptr, "En_Changer", "Bg_Jya_Megami", "Bg_Jya_Lift",
    "Bg_Jya_Bigmirror", "Bg_Jya_Bombchuiwa", "Bg_Jya_Amishutter", "Bg_Jya_Bombiwa",
    "Bg_Spot18_Basket", nullptr, "En_Ganon_Organ", "En_Siofuki", "En_Stream", nullptr, "En_Mm",
    "En_Ko", "En_Kz", "En_Weather_Tag", "Bg_Sst_Floor", "En_Ani", "En_Ex_Item",
    "Bg_Jya_Ironobj", "En_Js", "En_Jsjutan", "En_Cs", "En_Md", "En_Hy", "En_Ganon_Mant",
    "En_Okarina_Effect", "En_Mag", "Door_Gerudo", "Elf_Msg2", "Demo_Gt", "En_Po_Field",
    "Efc_Erupc", "Bg_Zg", "En_Heishi4", "En_Zl3", "Boss_Ganon2", "En_Kakasi", "En_Takara_Man",
    "Obj_Makeoshihiki", "Oceff_Spot", "End_Title", nullptr, "En_Torch", "Demo_Ec", "Shot_Sun",
    "En_Dy_Extra", "En_Wonder_Talk2", "En_Ge2", "Obj_Roomtimer", "En_Ssh", "En_Sth",
    "Oceff_Wipe", "Oceff_Storm", "En_Weiyer", "Bg_Spot05_Soko", "Bg_Jya_1flift",
    "Bg_Jya_Haheniron", "Bg_Spot12_Gate", "Bg_Spot12_Saku", "En_Hintnuts", "En_Nutsball",
    "Bg_Spot00_Break", "En_Shopnuts", "En_It", "En_Geldb", "Oceff_Wipe2", "Oceff_Wipe3",
    "En_Niw_Girl", "En_Dog", "En_Si", "Bg_Spot01_Objects2", "Obj_Comb", "Bg_Spot11_Bakudankabe",
    "Obj_Kibako2", "En_Dnt_Demo", "En_Dnt_Jiji", "En_Dnt_Nomal", "En_Guest", "Bg_Bom_Guard",
    "En_Hs2", "Demo_Kekkai", "Bg_Spot08_Bakudankabe", "Bg_Spot17_Bakudankabe", nullptr,
    "Obj_Mure3", "En_Tg", "En_Mu", "En_Go2", "En_Wf", "En_Skb", "Demo_Gj", "Demo_Geff",
    "Bg_Gnd_Firemeiro", "Bg_Gnd_Darkmeiro", "Bg_Gnd_Soulmeiro", "Bg_Gnd_Nisekabe",
    "Bg_Gnd_Iceblock", "En_Gb", "En_Gs", "Bg_Mizu_Bwall", "Bg_Mizu_Shutter",
    "En_Daiku_Kakariko", "Bg_Bowl_Wall", "En_Wall_Tubo", "En_Po_Desert", "En_Crow",
    "Door_Killer", "Bg_Spot11_Oasis", "Bg_Spot18_Futa", "Bg_Spot18_Shutter", "En_Ma3", "En_Cow",
    "Bg_Ice_Turara", "Bg_Ice_Shutter", "En_Kakasi2", "En_Kakasi3", "Oceff_Wipe4", "En_Eg",
    "Bg_Menkuri_Nisekabe", "En_Zo", "Obj_Makekinsuta", "En_Ge3", "Obj_Timeblock", "Obj_Hamishi",
    "En_Zl4", "En_Mm2", "Bg_Jya_Block", "Obj_Warp2block",
};

// nullptr for unnamed and out-of-table ids.
[[nodiscard]] inline const char *NativeActorName(uint16_t id) {
  return id < kNativeActorNames.size() ? kNativeActorNames[id] : nullptr;
}

// Actor categories (the actor list index, Actor+0x02; ACTORCAT_* of the same
// header).
inline constexpr std::array<const char *, 12> kNativeActorCategoryNames{
    "switch", "bg", "player", "explosive", "npc", "enemy",
    "prop", "itemaction", "misc", "boss", "door", "chest",
};

[[nodiscard]] inline const char *NativeActorCategoryName(uint8_t category) {
  return category < kNativeActorCategoryNames.size() ? kNativeActorCategoryNames[category] : nullptr;
}

} // namespace Oot3dNativeGame

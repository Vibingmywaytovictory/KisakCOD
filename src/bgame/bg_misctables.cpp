#include <universal/q_shared.h>
#include "bg_local.h"
#include "bg_public.h"
#include <qcommon/mem_track.h>

gitem_s bg_itemlist[MAX_ITEMLIST];
int itemRegistered[MAX_ITEMLIST] = { 0 };

void __cdecl TRACK_bg_misctables()
{
	track_static_alloc_internal(bg_itemlist, sizeof(bg_itemlist), "bg_itemlist", 9);
}

// Movement exploit toggles; registered in BG_RegisterDvars.
const dvar_t *pmove_fixed;
const dvar_t *pmove_msec;
const dvar_t *bg_strafeJumping;
const dvar_t *bg_bounces;

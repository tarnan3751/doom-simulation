/*
 * soc-layout.ld - hot code and data first in their output sections of sw/bsp/link.ld, for the
 * SoC's direct-mapped caches (passed with --section-ordering-file; why: README.md, "Cache
 * placement").  Placement only: no code or data changes.
 */
.text : {
    *(.text.init)
    /* walls and the BSP walk (R_RenderPlayerView -> R_RenderBSPNode) */
    *(.text.R_RenderBSPNode) *(.text.R_CheckBBox) *(.text.R_PointToAngle) *(.text.SlopeDiv)
    *(.text.R_Subsector) *(.text.R_FindPlane) *(.text.R_AddLine)
    *(.text.R_ClipSolidWallSegment) *(.text.R_ClipPassWallSegment) *(.text.R_StoreWallRange)
    *(.text.R_PointToDist) *(.text.R_ScaleFromGlobalAngle) *(.text.FixedDiv) *(.text.R_CheckPlane)
    *(.text.R_RenderSegLoop)
    /* used by every phase */
    *(.text.R_DrawColumn) *(.text.R_GetColumn) *(.text.W_CacheLumpNum) *(.text.Z_ChangeTag2)
    /* planes, sprites, presenting */
    *(.text.R_DrawPlanes) *(.text.R_MakeSpans) *(.text.R_MapPlane) *(.text.R_DrawSpan)
    *(.text.R_AddSprites) *(.text.R_ProjectSprite) *(.text.R_PointOnSide)
    *(.text.R_DrawMasked) *(.text.R_SortVisSprites) *(.text.R_DrawSprite) *(.text.R_DrawVisSprite)
    *(.text.R_DrawMaskedColumn) *(.text.R_DrawFuzzColumn) *(.text.R_RenderMaskedSegRange)
    *(.text.R_DrawPlayerSprites) *(.text.R_DrawPSprite) *(.text.DG_DrawFrame)
    /* status bar and HUD */
    *(.text.V_DrawPatch) *(.text.V_CopyRect) *(.text.memcpy) *(.text.memset) *(.text.M_AddToBox)
    *(.text.STlib_drawNum) *(.text.HUlib_drawTextLine) *(.text.V_DrawPatchDirect)
    /* the tic: thinkers, movement, sight */
    *(.text.P_RunThinkers) *(.text.P_MobjThinker) *(.text.P_SetMobjState) *(.text.P_CheckSight)
    *(.text.P_CrossBSPNode) *(.text.P_CrossSubsector) *(.text.P_DivlineSide)
    *(.text.R_PointInSubsector) *(.text.P_TryMove) *(.text.P_CheckPosition)
    *(.text.P_BlockLinesIterator) *(.text.PIT_CheckLine) *(.text.P_BlockThingsIterator)
    *(.text.PIT_CheckThing) *(.text.P_Move) *(.text.A_Chase) *(.text.P_LookForPlayers)
}

.sdata : {
    *(.sdata.validcount) *(.sdata.finecosine)
}

.sbss : {
    /* R_DrawColumn / R_DrawFuzzColumn, per column */
    *(.sbss.dc_x) *(.sbss.dc_yl) *(.sbss.dc_yh) *(.sbss.dc_iscale) *(.sbss.dc_texturemid)
    *(.sbss.dc_source) *(.sbss.dc_colormap) *(.sbss.viewbase) *(.sbss.centery) *(.sbss.fuzzpos)
    *(.sbss.colfunc) *(.sbss.viewheight) *(.sbss.viewwidth) *(.sbss.detailshift)
    /* R_RenderSegLoop / R_GetColumn / W_CacheLumpNum, per column */
    *(.sbss.rw_distance) *(.sbss.rw_offset) *(.sbss.rw_centerangle) *(.sbss.walllights)
    *(.sbss.maskedtexture) *(.sbss.maskedtexturecol) *(.sbss.rw_midtexturemid)
    *(.sbss.rw_toptexturemid) *(.sbss.rw_bottomtexturemid)
    *(.sbss.texturewidthmask) *(.sbss.texturecolumnlump) *(.sbss.texturecolumnofs)
    *(.sbss.texturecomposite) *(.sbss.lumpinfo) *(.sbss.numlumps)
    /* sprites, per column */
    *(.sbss.sprtopscreen) *(.sbss.spryscale) *(.sbss.mfloorclip) *(.sbss.mceilingclip)
    /* R_MapPlane / R_DrawSpan, per span */
    *(.sbss.ds_x1) *(.sbss.ds_x2) *(.sbss.ds_y) *(.sbss.ds_xfrac) *(.sbss.ds_yfrac)
    *(.sbss.ds_xstep) *(.sbss.ds_ystep) *(.sbss.ds_colormap) *(.sbss.ds_source) *(.sbss.spanfunc)
    *(.sbss.planeheight) *(.sbss.planezlight) *(.sbss.basexscale) *(.sbss.baseyscale)
    *(.sbss.fixedcolormap) *(.sbss.colormaps)
    /* the view */
    *(.sbss.viewx) *(.sbss.viewy) *(.sbss.viewz) *(.sbss.viewangle) *(.sbss.viewsin) *(.sbss.viewcos)
    *(.sbss.projection) *(.sbss.clipangle)
    /* per seg / per subsector */
    *(.sbss.rw_x) *(.sbss.rw_stopx) *(.sbss.rw_scale) *(.sbss.rw_scalestep) *(.sbss.rw_normalangle)
    *(.sbss.rw_angle1) *(.sbss.topfrac) *(.sbss.topstep) *(.sbss.bottomfrac) *(.sbss.bottomstep)
    *(.sbss.pixhigh) *(.sbss.pixlow) *(.sbss.pixhighstep) *(.sbss.pixlowstep)
    *(.sbss.worldtop) *(.sbss.worldbottom) *(.sbss.worldhigh) *(.sbss.worldlow)
    *(.sbss.markceiling) *(.sbss.markfloor) *(.sbss.segtextured)
    *(.sbss.midtexture) *(.sbss.toptexture) *(.sbss.bottomtexture)
    *(.sbss.ceilingplane) *(.sbss.floorplane) *(.sbss.lastvisplane) *(.sbss.lastopening)
    *(.sbss.ds_p) *(.sbss.curline) *(.sbss.frontsector) *(.sbss.backsector)
    *(.sbss.nodes) *(.sbss.lines) *(.sbss.segs) *(.sbss.subsectors) *(.sbss.numsubsectors)
    *(.sbss.skyflatnum) *(.sbss.dest_screen) *(.sbss.I_VideoBuffer)
}

.bss : {
    /* per column / per span */
    *(.bss.ceilingclip) *(.bss.floorclip) *(.bss.xtoviewangle) *(.bss.cliptop) *(.bss.clipbot)
    *(.bss.negonearray) *(.bss.screenheightarray) *(.bss.spanstart) *(.bss.cachedheight)
    *(.bss.cacheddistance) *(.bss.cachedxstep) *(.bss.cachedystep) *(.bss.distscale) *(.bss.yslope)
    /* per seg / per thing */
    *(.bss.solidsegs) *(.bss.strace) *(.bss.trace) *(.bss.tmbbox) *(.bss.dirtybox)
    *(.bss.players) *(.bss.playeringame)
}

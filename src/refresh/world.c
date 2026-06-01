/*
Copyright (C) 2003-2006 Andrey Nazarov

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "gl.h"

void GL_SampleLightPoint(vec3_t color)
{
    const mface_t       *surf = glr.lightpoint.surf;
    const byte          *lightmap;
    const byte          *b1, *b2, *b3, *b4;
    const lightstyle_t  *style;
    float               fracu, fracv;
    float               w1, w2, w3, w4;
    vec3_t              temp;
    int                 s, t, smax, tmax, size;

    s = glr.lightpoint.s;
    t = glr.lightpoint.t;

    fracu = glr.lightpoint.s - s;
    fracv = glr.lightpoint.t - t;

    // compute weights of lightmap blocks
    w1 = (1.0f - fracu) * (1.0f - fracv);
    w2 = fracu * (1.0f - fracv);
    w3 = fracu * fracv;
    w4 = (1.0f - fracu) * fracv;

    smax = surf->lm_width;
    tmax = surf->lm_height;
    size = smax * tmax * 3;

    VectorClear(color);

    // add all the lightmaps with bilinear filtering
    lightmap = surf->lightmap;
    for (int i = 0; i < surf->numstyles; i++) {
        b1 = &lightmap[3 * ((t + 0) * smax + (s + 0))];
        b2 = &lightmap[3 * ((t + 0) * smax + (s + 1))];
        b3 = &lightmap[3 * ((t + 1) * smax + (s + 1))];
        b4 = &lightmap[3 * ((t + 1) * smax + (s + 0))];

        temp[0] = w1 * b1[0] + w2 * b2[0] + w3 * b3[0] + w4 * b4[0];
        temp[1] = w1 * b1[1] + w2 * b2[1] + w3 * b3[1] + w4 * b4[1];
        temp[2] = w1 * b1[2] + w2 * b2[2] + w3 * b3[2] + w4 * b4[2];

        style = LIGHT_STYLE(surf->styles[i]);
        VectorMA(color, style->white, temp, color);

        lightmap += size;
    }
}

static bool GL_LightGridPoint(const lightgrid_t *grid, const vec3_t start, vec3_t ambient, vec3_t directed, vec3_t dir)
{
    vec3_t point, amb_avg;
    int32_t point_i[3];
    vec3_t amb_samples[8];
    int i, j, mask, numsamples;
    float style_boost[MAX_LIGHTMAPS];

    if (!grid->numleafs || !gl_lightgrid->integer)
        return false;

    point[0] = (start[0] - grid->mins[0]) * grid->scale[0];
    point[1] = (start[1] - grid->mins[1]) * grid->scale[1];
    point[2] = (start[2] - grid->mins[2]) * grid->scale[2];

    // If the point is outside the grid bounds, unsigned conversion would wrap
    // and cause a failed lookup. 
    if (point[0] < 0 || point[1] < 0 || point[2] < 0)
        return false;

    point_i[0] = (int32_t)floorf(point[0]);
    point_i[1] = (int32_t)floorf(point[1]);
    point_i[2] = (int32_t)floorf(point[2]);

    VectorClear(amb_avg);

    // Pre-calculate style intensity with overbright boost (2.0) once per call
    for (j = 0; j < grid->numstyles; j++) {
        const lightstyle_t *ls = LIGHT_STYLE(j);
        style_boost[j] = ls->white * 2.0f;
    }

    // Prepare clamped indices for the 8 surrounding grid points once
    uint32_t px[2], py[2], pz[2];
    px[0] = min((uint32_t)max(point_i[0], 0), grid->size[0] - 1);
    px[1] = min((uint32_t)max(point_i[0] + 1, 0), grid->size[0] - 1);
    py[0] = min((uint32_t)max(point_i[1], 0), grid->size[1] - 1);
    py[1] = min((uint32_t)max(point_i[1] + 1, 0), grid->size[1] - 1);
    pz[0] = min((uint32_t)max(point_i[2], 0), grid->size[2] - 1);
    pz[1] = min((uint32_t)max(point_i[2] + 1, 0), grid->size[2] - 1);

    for (i = mask = numsamples = 0; i < 8; i++) {
        uint32_t tmp[3];

        tmp[0] = px[(i >> 0) & 1];
        tmp[1] = py[(i >> 1) & 1];
        tmp[2] = pz[(i >> 2) & 1];

        const lightgrid_sample_t *s = BSP_LookupLightgrid(grid, tmp);
        if (!s)
            continue;

        VectorClear(amb_samples[i]);

        for (j = 0; j < grid->numstyles && s->rgb[0] != 255; j++, s++) {
            vec3_t rgb = { (float)s->rgb[0], (float)s->rgb[1], (float)s->rgb[2] };
            VectorMA(amb_samples[i], style_boost[j], rgb, amb_samples[i]);
        }

        // count non-occluded samples
        if (j) {
            mask |= BIT(i);
            VectorAdd(amb_avg, amb_samples[i], amb_avg);
            numsamples++;
        }
    }

    if (!mask)
        return false;

    // replace occluded samples with average
    if (mask != 255) {
        VectorScale(amb_avg, 1.0f / numsamples, amb_avg);
        for (i = 0; i < 8; i++)
            if (!(mask & BIT(i))) {
                VectorCopy(amb_avg, amb_samples[i]);
            }
    }

    // trilinear interpolation
    float fx, fy, fz;
    vec3_t amb_lerp_x[4], amb_lerp_y[2], final_amb;

    fx = point[0] - point_i[0];
    fy = point[1] - point_i[1];
    fz = point[2] - point_i[2];

    // Use smoothstep-based weights for trilinear interpolation to ensure C1 continuity.
    // This provides smoother transitions for both intensity and direction at boundaries.
    float sx = fx * fx * (3.0f - 2.0f * fx);
    float sy = fy * fy * (3.0f - 2.0f * fy);
    float sz = fz * fz * (3.0f - 2.0f * fz);
    float bsx = 1.0f - sx, bsy = 1.0f - sy, bsz = 1.0f - sz;

    LerpVector2(amb_samples[0], amb_samples[1], bsx, sx, amb_lerp_x[0]);
    LerpVector2(amb_samples[2], amb_samples[3], bsx, sx, amb_lerp_x[1]);
    LerpVector2(amb_samples[4], amb_samples[5], bsx, sx, amb_lerp_x[2]);
    LerpVector2(amb_samples[6], amb_samples[7], bsx, sx, amb_lerp_x[3]);
    LerpVector2(amb_lerp_x[0], amb_lerp_x[1], bsy, sy, amb_lerp_y[0]);
    LerpVector2(amb_lerp_x[2], amb_lerp_x[3], bsy, sy, amb_lerp_y[1]);
    LerpVector2(amb_lerp_y[0], amb_lerp_y[1], bsz, sz, final_amb);

    if (directed && dir) {
        // Determine direction via weighted average of points near the model.
        // We use the derivative of the smoothed trilinear interpolant for a continuous direction field.
        float l[8];
        vec3_t g, color;

        for (i = 0; i < 8; i++)
            l[i] = LUMINANCE(amb_samples[i][0], amb_samples[i][1], amb_samples[i][2]);

        VectorCopy(final_amb, color);

        // Scale derivatives by 6t(1-t)
        float dsx = 6.0f * fx * (1.0f - fx);
        float dsy = 6.0f * fy * (1.0f - fy);
        float dsz = 6.0f * fz * (1.0f - fz);

        g[0] = (((l[1] - l[0]) * bsy + (l[3] - l[2]) * sy) * bsz + ((l[5] - l[4]) * bsy + (l[7] - l[6]) * sy) * sz) * dsx;
        g[1] = (((l[2] - l[0]) * bsx + (l[3] - l[1]) * sx) * bsz + ((l[6] - l[4]) * bsx + (l[7] - l[5]) * sx) * sz) * dsy;
        g[2] = (((l[4] - l[0]) * bsx + (l[5] - l[1]) * sx) * bsy + ((l[6] - l[2]) * bsx + (l[7] - l[3]) * sx) * sy) * dsz;

        // Smoothly transition between gradient-based direction and tilted fallback
        // to prevent "popping" when magnitude threshold is crossed.
        vec3_t fallback = { 0.577f, 0.707f, 0.577f };
        float mag = sqrtf(DotProduct(g, g));
        float factor = Q_clipf(mag * 200.0f, 0.0f, 1.0f); // Soft transition range

        LerpVector(fallback, g, factor, dir);
        VectorNormalize(dir);

        // Mix and normalize ambient/directed based on gradient strength (factor)
        float w_amb = 0.4f - 0.15f * factor; // Keep ambient as is
        float w_dir = (0.6f + 0.15f * factor) * 3.0f; // Increase directional multiplier
        float f_inv = 1.0f / (w_amb + w_dir);

        VectorScale(color, w_amb * f_inv, ambient);
        VectorScale(color, w_dir * f_inv, directed);

        GL_AdjustColor(ambient);
        GL_AdjustColor(directed);
    } else {
        VectorCopy(final_amb, ambient);
        GL_AdjustColor(ambient);
    }

    return true;
}

static bool GL_LightPoint_(const vec3_t start, vec3_t ambient, vec3_t directed, vec3_t dir)
{
    const bsp_t     *bsp = gl_static.world.cache;
    vec3_t          color;
    int             index;
    lightpoint_t    pt;
    vec3_t          end, mins, maxs;
    const entity_t  *ent;
    const mmodel_t  *model;
    const vec_t     *angles;

    if (!bsp || !bsp->lightmap)
        return false;

    end[0] = start[0];
    end[1] = start[1];
    end[2] = start[2] - 8192;

    // get base lightpoint from world
    BSP_LightPoint(&glr.lightpoint, start, end, bsp->nodes, gl_static.nolm_mask | SURF_TRANS_MASK);

    // trace to other BSP models
    for (ent = glr.ents.bmodels; ent; ent = ent->next) {
        index = ~ent->model;
        if (index < 1 || index >= bsp->nummodels)
            continue;

        model = &bsp->models[index];
        if (!model->numfaces)
            continue;

        // cull in X/Y plane
        if (!VectorEmpty(ent->angles)) {
            if (fabsf(start[0] - ent->origin[0]) > model->radius)
                continue;
            if (fabsf(start[1] - ent->origin[1]) > model->radius)
                continue;
            angles = ent->angles;
        } else {
            VectorAdd(model->mins, ent->origin, mins);
            VectorAdd(model->maxs, ent->origin, maxs);
            if (start[0] < mins[0] || start[0] > maxs[0])
                continue;
            if (start[1] < mins[1] || start[1] > maxs[1])
                continue;
            angles = NULL;
        }

        BSP_TransformedLightPoint(&pt, start, end, model->headnode,
                                  gl_static.nolm_mask | SURF_TRANS_MASK, ent->origin, angles);

        if (pt.fraction < glr.lightpoint.fraction)
            glr.lightpoint = pt;
    }

    LerpVector(start, end, glr.lightpoint.fraction, glr.lightpoint.pos);

    if (GL_LightGridPoint(&bsp->lightgrid, start, ambient, directed, dir))
        return true;

    if (!glr.lightpoint.surf)
        return false;

    GL_SampleLightPoint(color);

    if (directed && dir) {
        // lightmap based fallback - use surface normal for direction
        // Use more punchy split for better contrast
        float w_amb = 0.4f; // Keep ambient as is
        float w_dir = 1.8f; // Increase directional multiplier (1.5x original 1.2)
        float f_inv = 1.0f / (w_amb + w_dir);

        VectorScale(color, w_amb * f_inv, ambient);
        VectorScale(color, w_dir * f_inv, directed);
        
        // Fallback light direction when lightgrid is not available
        vec3_t default_tilted_light = { 0.577f, 0.707f, 0.577f }; // A general overhead light with a slight tilt
        
        // If the surface normal is mostly vertical, blend it with a tilted default light
        // to ensure some horizontal component for yaw rotation to affect.
        if (fabsf(glr.lightpoint.plane.normal[2]) > 0.9f) { // If normal is mostly Z-axis
            LerpVector(default_tilted_light, glr.lightpoint.plane.normal, 0.5f, dir); // Blend 50/50
            VectorNormalize(dir);
        } else {
            VectorCopy(glr.lightpoint.plane.normal, dir);
            if (glr.lightpoint.surf->drawflags & DSURF_PLANEBACK)
                VectorInverse(dir);
        }

        GL_AdjustColor(ambient);
        GL_AdjustColor(directed);
    } else {
        VectorCopy(color, ambient);
        GL_AdjustColor(ambient);
    }

    return true;
}

static void GL_MarkLights_r(const mnode_t *node, const dlight_t *light, uint64_t lightbit)
{
    mface_t *face;
    vec_t dot;
    int i;
    float cutoff = (gl_backend->use_per_pixel_lighting() ? 0 : DLIGHT_CUTOFF);

    while (node->plane) {
        dot = PlaneDiffFast(light->transformed, node->plane);
        if (dot > light->radius - cutoff) {
            node = node->children[0];
            continue;
        }
        if (dot < -light->radius + cutoff) {
            node = node->children[1];
            continue;
        }

        for (i = 0, face = node->firstface; i < node->numfaces; i++, face++) {
            if (face->drawflags & gl_static.nolm_mask)
                continue;
            if (face->dlightframe != glr.dlightframe) {
                face->dlightframe = glr.dlightframe;
                face->dlightbits = 0;
            }
            face->dlightbits |= lightbit;
        }

        GL_MarkLights_r(node->children[0], light, lightbit);
        node = node->children[1];
    }
}

static void GL_MarkLights(void)
{
    int i;
    dlight_t *light;

    glr.dlightframe++;

    for (i = 0, light = glr.fd.dlights; i < glr.fd.num_dlights; i++, light++) {
        VectorCopy(light->origin, light->transformed);
        GL_MarkLights_r(gl_static.world.cache->nodes, light, BIT_ULL(i));
    }
}

static void GL_TransformLights(const mmodel_t *model)
{
    int i;
    dlight_t *light;
    vec3_t temp;

    glr.dlightframe++;

    for (i = 0, light = glr.fd.dlights; i < glr.fd.num_dlights; i++, light++) {
        VectorSubtract(light->origin, glr.ent->origin, temp);
        VectorRotate(temp, glr.entaxis, light->transformed);
        GL_MarkLights_r(model->headnode, light, BIT_ULL(i));
    }
}

static void GL_AddLightsExt(const vec3_t origin, vec3_t ambient, vec3_t directed, vec3_t dir)
{
    vec3_t light_dir;
    dlight_t *light;
    float f, intensity;
    int i;

    for (i = 0, light = glr.fd.dlights; i < glr.fd.num_dlights; i++, light++) {
        f = light->radius - DLIGHT_CUTOFF - Distance(light->origin, origin);
        if (f > 0) {
            f *= (1.0f / 255);
            intensity = f * light->intensity;

            if (directed && dir) {
                VectorSubtract(light->origin, origin, light_dir);
                if (VectorNormalize(light_dir) > 0.001f) {
                    // Pull the light direction towards the dynamic source
                    VectorMA(dir, intensity * 0.5f, light_dir, dir);
                }
                // Distribute dynamic light between ambient and directed for better shape
                VectorMA(ambient,  intensity * 0.4f, light->color, ambient);
                VectorMA(directed, intensity * 1.8f, light->color, directed); // Increase dynamic directional multiplier
            } else {
                VectorMA(ambient, intensity, light->color, ambient);
            }
        }
    }

    if (dir)
        VectorNormalize(dir);
}

void GL_LightPoint(const vec3_t origin, vec3_t color)
{
    if (gl_fullbright->integer) {
        VectorSet(color, 1, 1, 1);
        return;
    }

    // get lighting from world
    if (!GL_LightPoint_(origin, color, NULL, NULL))
        VectorSet(color, 1, 1, 1);

    // add dynamic lights
    if (!gl_backend->use_per_pixel_lighting())
        GL_AddLightsExt(origin, color, NULL, NULL);
}

void GL_LightPointExt(const vec3_t origin, vec3_t ambient, vec3_t directed, vec3_t dir)
{
    if (gl_fullbright->integer) {
        VectorSet(ambient, 1, 1, 1);
        VectorClear(directed);
        VectorSet(dir, 0, 0, 1);
        return;
    }

    if (!GL_LightPoint_(origin, ambient, directed, dir)) {
        VectorSet(ambient, 1, 1, 1);
        VectorClear(directed);
        VectorSet(dir, 0, 0, 1);
    }

    // Smooth lighting transitions for entities (0.1s duration)
    if (glr.ent && glr.ent != &gl_world) {
        if (glr.ent->light_frame == glr.drawframe - 1) {
            float f = glr.fd.frametime / 0.1f;
            if (f > 1.0f) f = 1.0f;

            LerpVector(glr.ent->light_ambient, ambient, f, ambient);
            LerpVector(glr.ent->light_directed, directed, f, directed);
            if (dir) {
                // If previous direction was invalid/zero, skip direction lerp to avoid noise
                if (DotProduct(glr.ent->light_dir, glr.ent->light_dir) < 0.001f) {
                    VectorCopy(dir, glr.ent->light_dir);
                }

                LerpVector(glr.ent->light_dir, dir, f, dir);
                VectorNormalize(dir);
            }
        }
        VectorCopy(ambient, glr.ent->light_ambient);
        VectorCopy(directed, glr.ent->light_directed);
        if (dir) VectorCopy(dir, glr.ent->light_dir);
        glr.ent->light_frame = glr.drawframe;
    }

    if (!gl_backend->use_per_pixel_lighting())
        GL_AddLightsExt(origin, ambient, directed, dir);
}

void R_LightPoint(const vec3_t origin, vec3_t color)
{
    GL_LightPoint(origin, color);
}

static void GL_MarkLeaves(void)
{
    const bsp_t *bsp = gl_static.world.cache;
    const mleaf_t *leaf;
    visrow_t vis1, vis2;
    int i, cluster1, cluster2;
    vec3_t tmp;

    if (gl_lockpvs->integer)
        return;

    leaf = BSP_PointLeaf(bsp->nodes, glr.fd.vieworg);
    cluster1 = cluster2 = leaf->cluster;
    VectorCopy(glr.fd.vieworg, tmp);
    if (!leaf->contents[0])
        tmp[2] -= 16;
    else
        tmp[2] += 16;
    leaf = BSP_PointLeaf(bsp->nodes, tmp);
    if (!(leaf->contents[0] & CONTENTS_SOLID))
        cluster2 = leaf->cluster;

    if (cluster1 == glr.viewcluster1 && cluster2 == glr.viewcluster2)
        return;

    glr.visframe++;
    glr.viewcluster1 = cluster1;
    glr.viewcluster2 = cluster2;

    if (!bsp->vis || gl_novis->integer || cluster1 == -1) {
        // mark everything visible
        for (i = 0; i < bsp->numnodes; i++)
            bsp->nodes[i].visframe = glr.visframe;

        for (i = 0; i < bsp->numleafs; i++)
            bsp->leafs[i].visframe = glr.visframe;

        glr.nodes_visible = bsp->numnodes;
        return;
    }

    BSP_ClusterVis(bsp, &vis1, cluster1, DVIS_PVS);
    if (cluster1 != cluster2) {
        BSP_ClusterVis(bsp, &vis2, cluster2, DVIS_PVS);
        int longs = VIS_FAST_LONGS(bsp->visrowsize);
        for (i = 0; i < longs; i++)
            vis1.l[i] |= vis2.l[i];
    }

    glr.nodes_visible = 0;
    for (i = 0, leaf = bsp->leafs; i < bsp->numleafs; i++, leaf++) {
        cluster1 = leaf->cluster;
        if (cluster1 == -1)
            continue;
        if (!Q_IsBitSet(vis1.b, cluster1))
            continue;
        // mark parent nodes visible
        for (mnode_t *node = (mnode_t *)leaf; node && node->visframe != glr.visframe; node = node->parent) {
            node->visframe = glr.visframe;
            glr.nodes_visible++;
        }
    }
}

#define BACKFACE_EPSILON    0.01f

void GL_DrawBspModel(mmodel_t *model)
{
    mface_t *face;
    vec3_t bounds[2];
    vec_t dot;
    vec3_t transformed, temp;
    entity_t *ent = glr.ent;
    glCullResult_t cull;
    glStateBits_t skymask;
    int i;

    if (!model->numfaces)
        return;

    if (glr.entrotated) {
        cull = GL_CullSphere(ent->origin, model->radius);
        if (cull == CULL_OUT) {
            c.spheresCulled++;
            return;
        }
        if (cull == CULL_CLIP) {
            VectorCopy(model->mins, bounds[0]);
            VectorCopy(model->maxs, bounds[1]);
            cull = GL_CullLocalBox(ent->origin, bounds);
            if (cull == CULL_OUT) {
                c.rotatedBoxesCulled++;
                return;
            }
        }
        VectorSubtract(glr.fd.vieworg, ent->origin, temp);
        VectorRotate(temp, glr.entaxis, transformed);
    } else {
        VectorAdd(model->mins, ent->origin, bounds[0]);
        VectorAdd(model->maxs, ent->origin, bounds[1]);
        cull = GL_CullBox(bounds);
        if (cull == CULL_OUT) {
            c.boxesCulled++;
            return;
        }
        VectorSubtract(glr.fd.vieworg, ent->origin, transformed);
    }

    GL_TransformLights(model);

    GL_RotateForEntity();

    skymask = gl_static.use_bmodel_skies ? GLS_SKY_MASK : 0;

    GL_BindArrays(VA_3D);

    GL_ClearSolidFaces();

    // draw visible faces
    for (i = 0, face = model->firstface; i < model->numfaces; i++, face++) {
        // sky faces don't have their polygon built
        if (face->drawflags & SURF_SKY && !(face->statebits & skymask))
            continue;
        if (face->drawflags & SURF_NODRAW)
            continue;

        dot = PlaneDiffFast(transformed, face->plane);
        if ((face->drawflags & DSURF_PLANEBACK) ? (dot > BACKFACE_EPSILON) : (dot < -BACKFACE_EPSILON)) {
            c.facesCulled++;
            continue;
        }

        if (gl_dynamic->integer)
            GL_PushLights(face);

        if (face->drawflags & SURF_TRANS_MASK) {
            if (model->drawframe != glr.drawframe)
                GL_AddAlphaFace(face);
            continue;
        }

        GL_AddSolidFace(face);
    }

    if (gl_dynamic->integer)
        GL_UploadLightmaps();

    GL_DrawSolidFaces();

    GL_Flush3D();

    // protect against infinite loop if the same inline model
    // with alpha faces is referenced by multiple entities
    model->drawframe = glr.drawframe;
}

#define NODE_CLIPPED    0
#define NODE_UNCLIPPED  MASK(4)

static inline bool GL_ClipNode(const mnode_t *node, int *clipflags)
{
    int flags = *clipflags;
    box_plane_t bits;

    if (flags == NODE_UNCLIPPED)
        return true;

    for (int i = 0, mask = 1; i < 4; i++, mask <<= 1) {
        if (flags & mask)
            continue;
        bits = BoxOnPlaneSide(node->mins, node->maxs,
                              &glr.frustumPlanes[i]);
        if (bits == BOX_BEHIND)
            return false;
        if (bits == BOX_INFRONT)
            flags |= mask;
    }

    *clipflags = flags;
    return true;
}

static inline void GL_DrawLeaf(const mleaf_t *leaf)
{
    // FIXME: use `&' here?
    if (leaf->contents[0] == CONTENTS_SOLID)
        return; // solid leaf

    if (glr.fd.areabits && !Q_IsBitSet(glr.fd.areabits, leaf->area))
        return; // door blocks sight

    for (int i = 0; i < leaf->numleaffaces; i++)
        leaf->firstleafface[i]->drawframe = glr.drawframe;

    c.leavesDrawn++;
}

static inline void GL_DrawNode(const mnode_t *node)
{
    mface_t *face;
    int i;

    for (i = 0, face = node->firstface; i < node->numfaces; i++, face++) {
        if (face->drawframe != glr.drawframe)
            continue;

        if (face->drawflags & SURF_SKY && !(face->statebits & GLS_SKY_MASK)) {
            R_AddSkySurface(face);
            continue;
        }

        if (face->drawflags & SURF_NODRAW)
            continue;

        if (gl_dynamic->integer)
            GL_PushLights(face);

        if (face->drawflags & SURF_TRANS_MASK)
            GL_AddAlphaFace(face);
        else
            GL_AddSolidFace(face);
    }

    c.nodesDrawn++;
}

static void GL_WorldNode_r(const mnode_t *node, int clipflags)
{
    int side;
    vec_t dot;

    while (node->visframe == glr.visframe) {
        if (!GL_ClipNode(node, &clipflags)) {
            c.nodesCulled++;
            break;
        }

        if (!node->plane) {
            GL_DrawLeaf((const mleaf_t *)node);
            break;
        }

        dot = PlaneDiffFast(glr.fd.vieworg, node->plane);
        side = dot < 0;

        GL_WorldNode_r(node->children[side], clipflags);

        GL_DrawNode(node);

        node = node->children[side ^ 1];
    }
}

void GL_DrawWorld(void)
{
    // auto cycle the world frame for texture animation
    gl_world.frame = (int)(glr.fd.time * 2);

    glr.ent = &gl_world;

    tess.dlight_bits = 0;

    GL_MarkLeaves();

    GL_MarkLights();

    R_ClearSkyBox();

    GL_LoadMatrix(gl_identity, glr.viewmatrix);

    GL_BindArrays(VA_3D);

    GL_ClearSolidFaces();

    GL_WorldNode_r(gl_static.world.cache->nodes,
                   gl_cull_nodes->integer ? NODE_CLIPPED : NODE_UNCLIPPED);

    if (gl_dynamic->integer)
        GL_UploadLightmaps();

    GL_DrawSolidFaces();

    GL_Flush3D();

    R_DrawSkyBox();
}

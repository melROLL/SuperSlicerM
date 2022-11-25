#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "../Surface.hpp"
#include "../ExtrusionEntity.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Geometry/MedialAxis.hpp"

#include "FillConcentric.hpp"

namespace Slic3r {

void
FillConcentric::init_spacing(coordf_t spacing, const FillParams &params)
{
    Fill::init_spacing(spacing, params);
    if (params.density > 0.9999f && !params.dont_adjust) {
        this->spacing_priv = unscaled(this->_adjust_solid_spacing(bounding_box.size()(0), _line_spacing_for_density(params.density)));
    }
}

void
FillConcentric::_fill_surface_single(
    const FillParams                &params, 
    unsigned int                     thickness_layers,
    const std::pair<float, Point>   &direction, 
    ExPolygon                        expolygon,
    Polylines                       &polylines_out) const
{
    // no rotation is supported for this infill pattern
    BoundingBox bounding_box = expolygon.contour.bounding_box();
    
    coord_t distance = _line_spacing_for_density(params.density);
    if (params.density > 0.9999f && !params.dont_adjust) {
        //it's == this->_adjust_solid_spacing(bounding_box.size()(0), _line_spacing_for_density(params.density)) because of the init_spacing()
        distance = scale_(this->get_spacing());
    }

    Polygons   loops = to_polygons(expolygon);
    ExPolygons last { std::move(expolygon) };
    while (! last.empty()) {
        last = offset2_ex(last, -double(distance + scale_(this->get_spacing()) /2), +double(scale_(this->get_spacing()) /2));
        append(loops, to_polygons(last));
    }

    // generate paths from the outermost to the innermost, to avoid
    // adhesion problems of the first central tiny loops
    loops = union_pt_chained_outside_in(loops);
    
    // split paths using a nearest neighbor search
    size_t iPathFirst = polylines_out.size();
    Point last_pos(0, 0);
    for (const Polygon &loop : loops) {
        polylines_out.emplace_back(loop.split_at_index(last_pos.nearest_point_index(loop.points)));
        last_pos = polylines_out.back().last_point();
    }

    // clip the paths to prevent the extruder from getting exactly on the first point of the loop
    // Keep valid paths only.
    size_t j = iPathFirst;
    for (size_t i = iPathFirst; i < polylines_out.size(); ++ i) {
        polylines_out[i].clip_end(double(this->loop_clipping));
        if (polylines_out[i].is_valid()) {
            if (j < i)
                polylines_out[j] = std::move(polylines_out[i]);
            ++ j;
        }
    }
    if (j < polylines_out.size())
        polylines_out.erase(polylines_out.begin() + j, polylines_out.end());
    //TODO: return ExtrusionLoop objects to get better chained paths,
    // otherwise the outermost loop starts at the closest point to (0, 0).
    // We want the loops to be split inside the G-code generator to get optimum path planning.
}

void
FillConcentricWGapFill::fill_surface_extrusion(
    const Surface *surface, 
    const FillParams &params,
    ExtrusionEntitiesPtr &out) const {

    double min_gapfill_area = double(params.flow.scaled_width()) * double(params.flow.scaled_width());
    if (params.config != nullptr) min_gapfill_area = scale_d(params.config->gap_fill_min_area.get_abs_value(params.flow.width())) * double(params.flow.scaled_width());
    // Perform offset.
    Slic3r::ExPolygons expp = offset_ex(surface->expolygon, double(scale_(0 - 0.5 * this->get_spacing())));
    // Create the infills for each of the regions.
    Polylines polylines_out;
    for (size_t i = 0; i < expp.size(); ++i) {
        //_fill_surface_single(
        //params,
        //surface->thickness_layers,
        //_infill_direction(surface),
        //expp[i],
        //polylines_out);
        ExPolygon expolygon = expp[i];

        coordf_t init_spacing = this->get_spacing();

        // no rotation is supported for this infill pattern
        BoundingBox bounding_box = expolygon.contour.bounding_box();

        coord_t distance = _line_spacing_for_density(params.density);
        if (params.density > 0.9999f && !params.dont_adjust) {
            distance = scale_t(this->get_spacing());
        }

        ExPolygons gaps;
        Polygons   loops = to_polygons(expolygon);
        ExPolygons last = { expolygon };
        bool first = true;
        while (!last.empty()) {
            ExPolygons next_onion = offset2_ex(last, -(distance + scale_d(this->get_spacing()) / 2), +(scale_d(this->get_spacing()) / 2));
            append(loops, to_polygons(next_onion));
            append(gaps, diff_ex(
                offset_ex(last, -0.5f * distance),
                offset_ex(next_onion, 0.5f * distance + 10)));  // safety offset                
            last = next_onion;
            if (first && !this->no_overlap_expolygons.empty()) {
                gaps = intersection_ex(gaps, this->no_overlap_expolygons);
            }
            first = false;
        }

        // generate paths from the outermost to the innermost, to avoid
        // adhesion problems of the first central tiny loops
        //note: useless if we don't apply no_sort flag
        //loops = union_pt_chained(loops, false);


        //get the role
        ExtrusionRole good_role = getRoleFromSurfaceType(params, surface);

        ExtrusionEntityCollection *coll_nosort = new ExtrusionEntityCollection();
        coll_nosort->set_can_sort_reverse(false, false); //can be sorted inside the pass
        extrusion_entities_append_loops(
            coll_nosort->set_entities(), loops,
            good_role,
            params.flow.mm3_per_mm() * params.flow_mult,
            params.flow.width() * params.flow_mult,
            params.flow.height());

        //add gapfills
        if (!gaps.empty() && params.density >= 1) {
            // collapse 
            coordf_t min = 0.2 * distance * (1 - INSET_OVERLAP_TOLERANCE);
            coordf_t max = 2. * distance;
            ExPolygons gaps_ex = diff_ex(
                offset2_ex(gaps, -min / 2, +min / 2),
                offset2_ex(gaps, -max / 2, +max / 2),
                ApplySafetyOffset::Yes);
            ThickPolylines polylines;
            for (const ExPolygon &ex : gaps_ex) {
                //remove too small gaps that are too hard to fill.
                //ie one that are smaller than an extrusion with width of min and a length of max.
                if (ex.area() > min_gapfill_area) {
                    Geometry::MedialAxis{ ex, coord_t(max), coord_t(min), scale_t(params.flow.height()) }.build(polylines);
                }
            }
            if (!polylines.empty() && !is_bridge(good_role)) {
                ExtrusionEntitiesPtr gap_fill_entities = Geometry::thin_variable_width(polylines, erGapFill, params.flow, scale_t(params.config->get_computed_value("resolution_internal")));
                if (!gap_fill_entities.empty()) {
                    //set role if needed
                    if (good_role != erSolidInfill) {
                        ExtrusionSetRole set_good_role(good_role);
                        for (ExtrusionEntity* ptr : gap_fill_entities)
                            ptr->visit(set_good_role);
                    }
                    //move them into the collection
                    coll_nosort->append(std::move(gap_fill_entities));
                }
            }
        }

        if (!coll_nosort->entities().empty())
            out.push_back(coll_nosort);
        else delete coll_nosort;
    }

    // external gapfill
    ExPolygons gapfill_areas = diff_ex(ExPolygons{ surface->expolygon }, offset_ex(expp, double(scale_(0.5 * this->get_spacing()))));
    gapfill_areas = union_safety_offset_ex(gapfill_areas);
    if (gapfill_areas.size() > 0) {
        double minarea = double(params.flow.scaled_width()) * double(params.flow.scaled_width());
        if (params.config != nullptr) minarea = scale_d(params.config->gap_fill_min_area.get_abs_value(params.flow.width())) * double(params.flow.scaled_width());
        for (int i = 0; i < gapfill_areas.size(); i++) {
            if (gapfill_areas[i].area() < minarea) {
                gapfill_areas.erase(gapfill_areas.begin() + i);
                i--;
            }
        }
        FillParams params2{ params };
        params2.role = erGapFill;

        do_gap_fill(intersection_ex(gapfill_areas, no_overlap_expolygons), params2, out);
    }

}

} // namespace Slic3r

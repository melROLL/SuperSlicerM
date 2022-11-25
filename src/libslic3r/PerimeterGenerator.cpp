#include "PerimeterGenerator.hpp"

#include "BridgeDetector.hpp"
#include "ClipperUtils.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Geometry.hpp"
#include "ShortestPath.hpp"
#include <cmath>
#include <cassert>
#include <vector>

#include "BoundingBox.hpp"
#include "ExPolygon.hpp"
#include "Geometry.hpp"
#include "Geometry/MedialAxis.hpp"
#include "Milling/MillingPostProcess.hpp"
#include "Polygon.hpp"
#include "Line.hpp"
#include "ClipperUtils.hpp"
#include "SVG.hpp"
#include <algorithm>
#include <cassert>
#include <list>
#include <boost/log/trivial.hpp>

namespace Slic3r {
#if _DEBUG
struct LoopAssertVisitor : public ExtrusionVisitorRecursiveConst {
    virtual void default_use(const ExtrusionEntity& entity) override {};
    virtual void use(const ExtrusionLoop& loop) override {
        for (auto it = std::next(loop.paths.begin()); it != loop.paths.end(); ++it) {
            assert(it->polyline.points.size() >= 2);
            assert(std::prev(it)->polyline.last_point() == it->polyline.first_point());
        }
        assert(loop.paths.front().first_point() == loop.paths.back().last_point());
    }
};
#endif
PerimeterGeneratorLoops get_all_Childs(PerimeterGeneratorLoop loop) {
    PerimeterGeneratorLoops ret;
    for (PerimeterGeneratorLoop &child : loop.children) {
        ret.push_back(child);
        PerimeterGeneratorLoops vals = get_all_Childs(child);
        ret.insert(ret.end(), vals.begin(), vals.end());
    }
    return ret;
}

// Thanks Cura developers for this function.
static void fuzzy_polygon(Polygon& poly, coordf_t fuzzy_skin_thickness, coordf_t fuzzy_skin_point_dist)
{
    const double min_dist_between_points = fuzzy_skin_point_dist * 3. / 4.; // hardcoded: the point distance may vary between 3/4 and 5/4 the supplied value
    const double range_random_point_dist = fuzzy_skin_point_dist / 2.;
    double dist_left_over = double(rand()) * (min_dist_between_points / 2) / double(RAND_MAX); // the distance to be traversed on the line before making the first new point
    Point* p0 = &poly.points.back();
    Points out;
    out.reserve(poly.points.size());
    for (Point& p1 : poly.points)
    { // 'a' is the (next) new point between p0 and p1
        Vec2d  p0p1 = (p1 - *p0).cast<double>();
        double p0p1_size = p0p1.norm();
        // so that p0p1_size - dist_last_point evaulates to dist_left_over - p0p1_size
        double dist_last_point = dist_left_over + p0p1_size * 2.;
        for (double p0pa_dist = dist_left_over; p0pa_dist < p0p1_size;
            p0pa_dist += min_dist_between_points + double(rand()) * range_random_point_dist / double(RAND_MAX))
        {
            double r = double(rand()) * (fuzzy_skin_thickness * 2.) / double(RAND_MAX) - fuzzy_skin_thickness;
            out.emplace_back(*p0 + (p0p1 * (p0pa_dist / p0p1_size) + perp(p0p1).cast<double>().normalized() * r).cast<coord_t>());
            dist_last_point = p0pa_dist;
        }
        dist_left_over = p0p1_size - dist_last_point;
        p0 = &p1;
    }
    while (out.size() < 3) {
        size_t point_idx = poly.size() - 2;
        out.emplace_back(poly[point_idx]);
        if (point_idx == 0)
            break;
        --point_idx;
    }
    if (out.size() >= 3)
        poly.points = std::move(out);
}

void PerimeterGenerator::process()
{
    // other perimeters
    this->m_mm3_per_mm                  = this->perimeter_flow.mm3_per_mm();
    const coord_t perimeter_width       = this->perimeter_flow.scaled_width();
    //spacing between internal perimeters
    const coord_t perimeter_spacing     = this->perimeter_flow.scaled_spacing();
    
    // external perimeters
    this->m_ext_mm3_per_mm              = this->ext_perimeter_flow.mm3_per_mm();
    const coord_t ext_perimeter_width   = this->ext_perimeter_flow.scaled_width();
    //spacing between two external perimeter (where you don't have the space to add other loops)
    const coord_t ext_perimeter_spacing = this->ext_perimeter_flow.scaled_spacing();
    //spacing between external perimeter and the second
    coord_t ext_perimeter_spacing2      = ext_perimeter_spacing / 2 + perimeter_spacing / 2; //this->ext_perimeter_flow.scaled_spacing(this->perimeter_flow);

    // overhang perimeters
    this->m_mm3_per_mm_overhang         = this->overhang_flow.mm3_per_mm();

    //gap fill
    const coord_t gap_fill_spacing_external = this->config->gap_fill_overlap.get_abs_value(this->ext_perimeter_flow.with_spacing_ratio(1).scaled_spacing())
        + this->ext_perimeter_flow.scaled_width() * (1 - this->config->gap_fill_overlap.get_abs_value(1.));
    const coord_t gap_fill_spacing      = this->config->gap_fill_overlap.get_abs_value(this->perimeter_flow.with_spacing_ratio(1).scaled_spacing())
        + this->perimeter_flow.scaled_width() * (1 - this->config->gap_fill_overlap.get_abs_value(1.));

    // solid infill
    const coord_t solid_infill_spacing  = this->solid_infill_flow.scaled_spacing();

    //infill / perimeter
    coord_t infill_peri_overlap         = (coord_t)scale_(this->config->get_abs_value("infill_overlap", unscale<coordf_t>(perimeter_spacing + solid_infill_spacing) / 2));
    // infill gap to add vs perimeter (useful if using perimeter bonding)
    coord_t infill_gap                  = 0;

    const bool round_peri = this->config->perimeter_round_corners.value;
    const coord_t min_round_spacing = perimeter_width / 10;

    // perimeter bonding set.
    if (this->perimeter_flow.spacing_ratio() == 1
        && this->ext_perimeter_flow.spacing_ratio() == 1
        && this->config->external_perimeters_first
        && this->object_config->perimeter_bonding.value > 0) {
        infill_gap = (1 - this->object_config->perimeter_bonding.get_abs_value(1)) * ext_perimeter_spacing;
        ext_perimeter_spacing2 -= infill_gap;
    }
    
    // Calculate the minimum required spacing between two adjacent traces.
    // This should be equal to the nominal flow spacing but we experiment
    // with some tolerance in order to avoid triggering medial axis when
    // some squishing might work. Loops are still spaced by the entire
    // flow spacing; this only applies to collapsing parts.
    // For ext_min_spacing we use the ext_perimeter_spacing calculated for two adjacent
    // external loops (which is the correct way) instead of using ext_perimeter_spacing2
    // which is the spacing between external and internal, which is not correct
    // and would make the collapsing (thus the details resolution) dependent on 
    // internal flow which is unrelated. <- i don't undertand, so revert to ext_perimeter_spacing2
    //const coord_t min_spacing     = (coord_t)( perimeter_spacing      * (1 - 0.05/*INSET_OVERLAP_TOLERANCE*/) );
    //const coord_t ext_min_spacing = (coord_t)( ext_perimeter_spacing2  * (1 - 0.05/*INSET_OVERLAP_TOLERANCE*/) );
    // now the tolerance is built in thin_periemter settings
    
    // prepare grown lower layer slices for overhang detection
    if (this->lower_slices != NULL && (this->config->overhangs_width.value > 0 || this->config->overhangs_width_speed.value > 0)) {
        // We consider overhang any part where the entire nozzle diameter is not supported by the
        // lower layer, so we take lower slices and offset them by overhangs_width of the nozzle diameter used 
        // in the current layer

        //we use a range to avoid threshold issues.
        coord_t overhangs_width_flow = scale_t(config->overhangs_width.get_abs_value(this->overhang_flow.nozzle_diameter()));
        coord_t overhangs_width_speed = scale_t(config->overhangs_width_speed.get_abs_value(this->overhang_flow.nozzle_diameter()));
        coord_t min_feature = std::min(overhangs_width_flow, overhangs_width_speed) / 10;
        coord_t overhangs_width_flow_90 = coord_t(overhangs_width_flow * 0.99);
        coord_t overhangs_width_flow_110 = coord_t(overhangs_width_flow * 1.15);
        coord_t overhangs_width_speed_90 = coord_t(overhangs_width_speed * 0.99);
        coord_t overhangs_width_speed_110 = coord_t(overhangs_width_speed * 1.15);

        //flow offset should be greater than speed offset because the flow apply also the speed.
        //check if overhangs_width_speed is low enough to be relevant (if flow is activated)
        if (overhangs_width_flow > 0 && overhangs_width_speed + this->overhang_flow.nozzle_diameter() * 0.01 > overhangs_width_flow) {
            overhangs_width_speed = 0;
            overhangs_width_speed_90 = 0;
            overhangs_width_speed_110 = 0;
        }
        if (overhangs_width_flow > 0) {
            if (overhangs_width_flow_90 < overhangs_width_speed_110) {
                overhangs_width_speed_110 = overhangs_width_flow_90 = (overhangs_width_flow + overhangs_width_speed) / 2;
            }
        }

        if (overhangs_width_speed > 0 || overhangs_width_flow > 0) {
            ExPolygons simplified;
            //simplify the lower slices if too high (means low number) resolution (we can be very aggressive here)
            if (this->print_config->resolution < min_feature / 2) {
                for (const ExPolygon& expoly : *lower_slices) {
                    expoly.simplify(min_feature, &simplified);
                }
            }

            if (overhangs_width_speed > 0 && (overhangs_width_speed < overhangs_width_flow || overhangs_width_flow == 0)) {
                this->_lower_slices_bridge_speed_small = offset((simplified.empty() ? *this->lower_slices : simplified), (coordf_t)overhangs_width_speed_90 - (coordf_t)(ext_perimeter_width / 2));
                this->_lower_slices_bridge_speed_big = offset((simplified.empty() ? *this->lower_slices : simplified), (coordf_t)overhangs_width_speed_110 - (coordf_t)(ext_perimeter_width / 2));
            }
            if (overhangs_width_flow > 0) {
                if (overhangs_width_speed_110 == overhangs_width_flow_90 && overhangs_width_speed < overhangs_width_flow)
                    this->_lower_slices_bridge_flow_small = this->_lower_slices_bridge_speed_big;
                else
                    this->_lower_slices_bridge_flow_small = offset((simplified.empty() ? *this->lower_slices : simplified), (coordf_t)overhangs_width_flow_90 - (coordf_t)(ext_perimeter_width / 2));
                this->_lower_slices_bridge_flow_big = offset((simplified.empty() ? *this->lower_slices : simplified), (coordf_t)overhangs_width_flow_110 - (coordf_t)(ext_perimeter_width / 2));
            }
        }
    }

    // have to grown the perimeters if mill post-process
    MillingPostProcess miller(this->slices, this->lower_slices, config, object_config, print_config);
    bool have_to_grow_for_miller =  miller.can_be_milled(layer) && config->milling_extra_size.get_abs_value(1) > 0;
    ExPolygons unmillable;
    coord_t mill_extra_size = 0;
    if (have_to_grow_for_miller) {
        unmillable = miller.get_unmillable_areas(layer);
        double spacing_vs_width = ext_perimeter_flow.width() - ext_perimeter_flow.spacing();
        mill_extra_size = scale_(config->milling_extra_size.get_abs_value(spacing_vs_width));
        have_to_grow_for_miller = mill_extra_size > SCALED_EPSILON;
    }

    
    // we need to process each island separately because we might have different
    // extra perimeters for each one
    int surface_idx = 0;
    Surfaces all_surfaces = this->slices->surfaces;

    //store surface for bridge infill to avoid unsupported perimeters (but the first one, this one is always good)
    if (this->config->no_perimeter_unsupported_algo != npuaNone
        && this->lower_slices != NULL && !this->lower_slices->empty()) {
        coordf_t bridged_infill_margin = scale_d(config->bridged_infill_margin.get_abs_value(unscaled(ext_perimeter_width)));

        for (surface_idx = 0; surface_idx < all_surfaces.size(); surface_idx++) {
            Surface *surface = &all_surfaces[surface_idx];
            ExPolygons last = { surface->expolygon };
            //compute our unsupported surface
            ExPolygons unsupported = diff_ex(last, *this->lower_slices, ApplySafetyOffset::Yes);
            if (!unsupported.empty()) {
                //remove small overhangs
                ExPolygons unsupported_filtered = offset2_ex(unsupported, double(-perimeter_spacing), double(perimeter_spacing));
                if (!unsupported_filtered.empty()) {
                    //to_draw.insert(to_draw.end(), last.begin(), last.end());
                    //extract only the useful part of the lower layer. The safety offset is really needed here.
                    ExPolygons support = diff_ex(last, unsupported, ApplySafetyOffset::Yes);
                    if (!unsupported.empty()) {
                        //only consider the part that can be bridged (really, by the bridge algorithm)
                        //first, separate into islands (ie, each ExPlolygon)
                        int numploy = 0;
                        //only consider the bottom layer that intersect unsupported, to be sure it's only on our island.
                        ExPolygonCollection lower_island(support);
                        //a detector per island
                        ExPolygons bridgeable;
                        for (ExPolygon unsupported : unsupported_filtered) {
                            BridgeDetector detector{ unsupported,
                                lower_island.expolygons,
                                perimeter_spacing };
                            if (detector.detect_angle(Geometry::deg2rad(this->config->bridge_angle.value)))
                                expolygons_append(bridgeable, union_ex(detector.coverage(-1, true)));
                        }
                        if (!bridgeable.empty()) {
                            //check if we get everything or just the bridgeable area
                            if (this->config->no_perimeter_unsupported_algo.value == npuaNoPeri || this->config->no_perimeter_unsupported_algo.value == npuaFilled) {
                                //we bridge everything, even the not-bridgeable bits
                                for (size_t i = 0; i < unsupported_filtered.size();) {
                                    ExPolygon &poly_unsupp = *(unsupported_filtered.begin() + i);
                                    Polygons contour_simplified = poly_unsupp.contour.simplify(perimeter_spacing);
                                    ExPolygon poly_unsupp_bigger = poly_unsupp;
                                    Polygons contour_bigger = offset(poly_unsupp_bigger.contour, bridged_infill_margin);
                                    if (contour_bigger.size() == 1) poly_unsupp_bigger.contour = contour_bigger[0];

                                    //check convex, has some bridge, not overhang
                                    if (contour_simplified.size() == 1 && contour_bigger.size() == 1 && contour_simplified[0].concave_points().size() == 0
                                        && intersection_ex(bridgeable, ExPolygons{ poly_unsupp }).size() > 0
                                        && diff_ex(ExPolygons{ poly_unsupp_bigger }, union_ex(last, offset_ex(bridgeable, bridged_infill_margin + perimeter_spacing / 2)), ApplySafetyOffset::Yes).size() == 0
                                        ) {
                                        //ok, keep it
                                        i++;
                                    } else {
                                        unsupported_filtered.erase(unsupported_filtered.begin() + i);
                                    }
                                }
                                unsupported_filtered = intersection_ex(last,
                                    offset2_ex(unsupported_filtered, double(-perimeter_spacing / 2), double(bridged_infill_margin + perimeter_spacing / 2)));
                                if (this->config->no_perimeter_unsupported_algo.value == npuaFilled) {
                                    for (ExPolygon &expol : unsupported_filtered) {
                                        //check if the holes won't be covered by the upper layer
                                        //TODO: if we want to do that, we must modify the geometry before making perimeters.
                                        //if (this->upper_slices != nullptr && !this->upper_slices->expolygons.empty()) {
                                        //    for (Polygon &poly : expol.holes) poly.make_counter_clockwise();
                                        //    float perimeterwidth = this->config->perimeters == 0 ? 0 : (this->ext_perimeter_flow.scaled_width() + (this->config->perimeters - 1) + this->perimeter_flow.scaled_spacing());
                                        //    std::cout << "test upper slices with perimeterwidth=" << perimeterwidth << "=>" << offset_ex(this->upper_slices->expolygons, -perimeterwidth).size();
                                        //    if (intersection(Polygons() = { expol.holes }, to_polygons(offset_ex(this->upper_slices->expolygons, -this->ext_perimeter_flow.scaled_width() / 2))).empty()) {
                                        //        std::cout << " EMPTY§";
                                        //        expol.holes.clear();
                                        //    } else {
                                        //    }
                                        //    std::cout << "\n";
                                        //} else {
                                            expol.holes.clear();
                                        //}

                                        //detect inside volume
                                        for (size_t surface_idx_other = 0; surface_idx_other < all_surfaces.size(); surface_idx_other++) {
                                            if (surface_idx == surface_idx_other) continue;
                                            if (intersection_ex(ExPolygons() = { expol }, ExPolygons() = { all_surfaces[surface_idx_other].expolygon }).size() > 0) {
                                                //this means that other_surf was inside an expol holes
                                                //as we removed them, we need to add a new one
                                                ExPolygons new_poly = offset2_ex(ExPolygons{ all_surfaces[surface_idx_other].expolygon }, double(-bridged_infill_margin - perimeter_spacing ), double(perimeter_spacing));
                                                if (new_poly.size() == 1) {
                                                    all_surfaces[surface_idx_other].expolygon = new_poly[0];
                                                    expol.holes.push_back(new_poly[0].contour);
                                                    expol.holes.back().make_clockwise();
                                                } else {
                                                    for (size_t idx = 0; idx < new_poly.size(); idx++) {
                                                        Surface new_surf = all_surfaces[surface_idx_other];
                                                        new_surf.expolygon = new_poly[idx];
                                                        all_surfaces.push_back(new_surf);
                                                        expol.holes.push_back(new_poly[idx].contour);
                                                        expol.holes.back().make_clockwise();
                                                    }
                                                    all_surfaces.erase(all_surfaces.begin() + surface_idx_other);
                                                    if (surface_idx_other < surface_idx) {
                                                        surface_idx--;
                                                        surface = &all_surfaces[surface_idx];
                                                    }
                                                    surface_idx_other--;
                                                }
                                            }
                                        }
                                    }

                                }
                                //TODO: add other polys as holes inside this one (-margin)
                            } else if (this->config->no_perimeter_unsupported_algo.value == npuaBridgesOverhangs || this->config->no_perimeter_unsupported_algo.value == npuaBridges){
                                //simplify to avoid most of artefacts from printing lines.
                                ExPolygons bridgeable_simplified;
                                for (ExPolygon &poly : bridgeable) {
                                    poly.simplify(perimeter_spacing, &bridgeable_simplified);
                                }
                                bridgeable_simplified = offset2_ex(bridgeable_simplified, -ext_perimeter_width, ext_perimeter_width);
                                //bridgeable_simplified = intersection_ex(bridgeable_simplified, unsupported_filtered);
                                //offset by perimeter spacing because the simplify may have reduced it a bit.
                                //it's not dangerous as it will be intersected by 'unsupported' later
                                //FIXME: add overlap in this->fill_surfaces->append
                                //FIXME: it overlap inside unsuppported not-bridgeable area!

                                //bridgeable_simplified = offset2_ex(bridgeable_simplified, (double)-perimeter_spacing, (double)perimeter_spacing * 2);
                                //ExPolygons unbridgeable = offset_ex(diff_ex(unsupported, bridgeable_simplified), perimeter_spacing * 3 / 2);
                                //ExPolygons unbridgeable = intersection_ex(unsupported, diff_ex(unsupported_filtered, offset_ex(bridgeable_simplified, ext_perimeter_width / 2)));
                                //unbridgeable = offset2_ex(unbridgeable, -ext_perimeter_width, ext_perimeter_width);


                                if (this->config->no_perimeter_unsupported_algo.value == npuaBridges) {
                                    ExPolygons unbridgeable = unsupported_filtered;
                                    for (ExPolygon &expol : unbridgeable)
                                        expol.holes.clear();
                                    unbridgeable = diff_ex(unbridgeable, bridgeable_simplified);
                                    unbridgeable = offset2_ex(unbridgeable, -ext_perimeter_width*2, ext_perimeter_width*2);
                                    ExPolygons bridges_temp = offset2_ex(intersection_ex(last, diff_ex(unsupported_filtered, unbridgeable), ApplySafetyOffset::Yes),-ext_perimeter_width/4, ext_perimeter_width/4);
                                    //remove the overhangs section from the surface polygons
                                    ExPolygons reference = last;
                                    last = diff_ex(last, unsupported_filtered);
                                    //ExPolygons no_bridge = diff_ex(offset_ex(unbridgeable, ext_perimeter_width * 3 / 2), last);
                                    //bridges_temp = diff_ex(bridges_temp, no_bridge);
                                    coordf_t offset_to_do = bridged_infill_margin;
                                    bool first = true;
                                    unbridgeable = diff_ex(unbridgeable, offset_ex(bridges_temp, ext_perimeter_width));
                                    while (offset_to_do > ext_perimeter_width * 1.5) {
                                        unbridgeable = offset2_ex(unbridgeable, -ext_perimeter_width/4, ext_perimeter_width*2.25, ClipperLib::jtSquare);
                                        bridges_temp = diff_ex(bridges_temp, unbridgeable);
                                        bridges_temp = offset_ex(bridges_temp, ext_perimeter_width, ClipperLib::jtMiter, 6.);
                                        unbridgeable = diff_ex(unbridgeable, offset_ex(bridges_temp, ext_perimeter_width));
                                        offset_to_do -= ext_perimeter_width;
                                        first = false;
                                    }
                                    unbridgeable = offset_ex(unbridgeable, ext_perimeter_width + offset_to_do, ClipperLib::jtSquare);
                                    bridges_temp = diff_ex(bridges_temp, unbridgeable);
                                    unsupported_filtered = offset_ex(bridges_temp, offset_to_do);
                                    unsupported_filtered = intersection_ex(unsupported_filtered, reference);
                                } else {
                                    ExPolygons unbridgeable = intersection_ex(unsupported, diff_ex(unsupported_filtered, offset_ex(bridgeable_simplified, ext_perimeter_width / 2)));
                                    unbridgeable = offset2_ex(unbridgeable, -ext_perimeter_width, ext_perimeter_width);
                                    unsupported_filtered = unbridgeable;

                                    ////put the bridge area inside the unsupported_filtered variable
                                    //unsupported_filtered = intersection_ex(last,
                                    //    diff_ex(
                                    //    offset_ex(bridgeable_simplified, (double)perimeter_spacing / 2),
                                    //    unbridgeable
                                    //    )
                                    //    );
                                }
                            } else {
                                unsupported_filtered.clear();
                            }
                        } else {
                            unsupported_filtered.clear();
                        }
                    }

                    if (!unsupported_filtered.empty()) {

                        //add this directly to the infill list.
                        // this will avoid to throw wrong offsets into a good polygons
                        this->fill_surfaces->append(
                            unsupported_filtered,
                            stPosInternal | stDensSparse);

                        // store the results
                        last = diff_ex(last, unsupported_filtered, ApplySafetyOffset::Yes);
                        //remove "thin air" polygons (note: it assumes that all polygons below will be extruded)
                        for (int i = 0; i < last.size(); i++) {
                            if (intersection_ex(support, ExPolygons() = { last[i] }).empty()) {
                                this->fill_surfaces->append(
                                    ExPolygons() = { last[i] },
                                    stPosInternal | stDensSparse);
                                last.erase(last.begin() + i);
                                i--;
                            }
                        }
                    }
                }
            }
            if (last.size() == 0) {
                all_surfaces.erase(all_surfaces.begin() + surface_idx);
                surface_idx--;
            } else {
                surface->expolygon = last[0];
                for (size_t idx = 1; idx < last.size(); idx++) {
                    all_surfaces.emplace_back(*surface, last[idx]);
                }
            }
        }
    }

    surface_idx = 0;
    const int extra_odd_perimeter = (config->extra_perimeters_odd_layers && layer->id() % 2 == 1 ? 1:0);
    for (const Surface& surface : all_surfaces) {
        // detect how many perimeters must be generated for this island
        int        loop_number = this->config->perimeters + surface.extra_perimeters - 1 + extra_odd_perimeter;  // 0-indexed loops
        surface_idx++;

        if ((layer->id() == 0 && this->config->only_one_perimeter_first_layer) || (this->config->only_one_perimeter_top && loop_number > 0 && this->upper_slices == NULL)) {
            loop_number = 0;
        }

        ExPolygons gaps;
        //this var store infill surface removed from last to not add any more perimeters to it.
        ExPolygons top_fills;
        ExPolygons fill_clip;
        // simplification already done at slicing
        //simplify the loop to avoid artifacts when shrinking almost-0 segments
        coord_t resolution = get_resolution(0, false, &surface);
        ExPolygons last    = union_ex(surface.expolygon.simplify_p((resolution < SCALED_EPSILON ? SCALED_EPSILON : resolution)));
        double last_area   = -1;

        if (loop_number >= 0) {

            //increase surface for milling_post-process
            if (have_to_grow_for_miller) {
                if (unmillable.empty())
                    last = offset_ex(last, mill_extra_size);
                else {
                //FIXME only work if mill_extra_size < mill_nozzle/2 (becasue it's the extra offset from unmillable)
                //FIXME overhangs if mill_extra_size is too big
                //FIXME merge with process_arachne?
                    ExPolygons growth = diff_ex(offset_ex(last, mill_extra_size), unmillable, ApplySafetyOffset::Yes);
                    last.insert(last.end(), growth.begin(), growth.end());
                    last = union_ex(last);
                }
            }


            // Add perimeters on overhangs : initialization
            ExPolygons overhangs_unsupported;
            if ((this->config->extra_perimeters_overhangs || (this->config->overhangs_reverse && this->layer->id() % 2 == 1))
                && !last.empty() && this->lower_slices != NULL && !this->lower_slices->empty()) {
                //remove holes from lower layer, we only ant that for overhangs, not bridges!
                ExPolygons lower_without_holes;
                for (const ExPolygon& exp : *this->lower_slices)
                    lower_without_holes.emplace_back(to_expolygon(exp.contour));
                // opening is offset2-+
                overhangs_unsupported = opening_ex(diff_ex(last, lower_without_holes, ApplySafetyOffset::Yes), scale_t(print_config->resolution_internal));
                if (!overhangs_unsupported.empty()) {
                    //only consider overhangs and let bridges alone
                    //only consider the part that can be bridged (really, by the bridge algorithm)
                    //first, separate into islands (ie, each ExPlolygon)
                    //only consider the bottom layer that intersect unsupported, to be sure it's only on our island.
                    const ExPolygonCollection lower_island(diff_ex(last, overhangs_unsupported));
                    ExPolygons bridgeable;
                    for (ExPolygon unsupported : overhangs_unsupported) {
                        BridgeDetector detector{ unsupported,
                            lower_island.expolygons,
                            perimeter_spacing };
                        if (detector.detect_angle(Geometry::deg2rad(this->config->bridge_angle.value)))
                            expolygons_append(bridgeable, union_ex(detector.coverage(-1, true)));
                    }
                    if (!bridgeable.empty()) {
                        //simplify to avoid most of artefacts from printing lines.
                        ExPolygons bridgeable_simplified;
                        for (const ExPolygon& poly : bridgeable) {
                            poly.simplify(perimeter_spacing / 2, &bridgeable_simplified);
                        }

                        //offset by perimeter spacing because the simplify may have reduced it a bit.
                        if (!bridgeable_simplified.empty()) {
                            bridgeable_simplified = offset_ex(bridgeable_simplified, double(perimeter_spacing));
                            overhangs_unsupported = diff_ex(overhangs_unsupported, bridgeable_simplified, ApplySafetyOffset::Yes);
                        }
                    }
                }
            }
            bool has_steep_overhang = false;
            if (this->layer->id() % 2 == 1 && this->config->overhangs_reverse //check if my option is set and good layer
                && !last.empty() && !overhangs_unsupported.empty() //has something to work with
                ) {
                coord_t offset = scale_t(config->overhangs_reverse_threshold.get_abs_value(this->perimeter_flow.width()));
                //version with °: scale_(std::tan(PI * (0.5f / 90) * config->overhangs_reverse_threshold.value ) * this->layer->height)

                if (offset_ex(overhangs_unsupported, -offset / 2.).size() > 0) {
                    //allow this loop to be printed in reverse
                    has_steep_overhang = true;
                }
            }

            // In case no perimeters are to be generated, loop_number will equal to -1.            
            std::vector<PerimeterGeneratorLoops> contours(loop_number + 1);    // depth => loops
            std::vector<PerimeterGeneratorLoops> holes(loop_number + 1);       // depth => loops
            ThickPolylines thin_walls_thickpolys;
            ExPolygons no_last_gapfill;
            // we loop one time more than needed in order to find gaps after the last perimeter was applied
            for (int perimeter_idx = 0;; ++perimeter_idx) {  // outer loop is 0

                // We can add more perimeters if there are uncovered overhangs
                // improvement for future: find a way to add perimeters only where it's needed.
                bool has_overhang = false;
                if (this->config->extra_perimeters_overhangs && !last.empty() && !overhangs_unsupported.empty()) {
                    overhangs_unsupported = intersection_ex(overhangs_unsupported, last, ApplySafetyOffset::Yes);
                    if (overhangs_unsupported.size() > 0) {
                        //please don't stop adding perimeter yet.
                        has_overhang = true;
                    }
                }

                // allow this perimeter to overlap itself?
                float thin_perimeter = this->config->thin_perimeters.get_abs_value(1);
                if (perimeter_idx > 0 && thin_perimeter != 0) {
                    thin_perimeter = this->config->thin_perimeters_all.get_abs_value(1);
                }
                bool allow_perimeter_anti_hysteresis = thin_perimeter >= 0;
                if (thin_perimeter < 0) {
                    thin_perimeter = -thin_perimeter;
                }
                if (thin_perimeter < 0.02) { // can create artifacts
                    thin_perimeter = 0;
                }

                // Calculate next onion shell of perimeters.
                //this variable stored the next onion
                ExPolygons next_onion;
                if (perimeter_idx == 0) {
                    // compute next onion
                        // the minimum thickness of a single loop is:
                        // ext_width/2 + ext_spacing/2 + spacing/2 + width/2
                    if (thin_perimeter > 0.98) {
                        next_onion = offset_ex(
                            last,
                            -(float)(ext_perimeter_width / 2),
                            ClipperLib::JoinType::jtMiter,
                            3);
                    } else if (thin_perimeter > 0.01) {
                        next_onion = offset2_ex(
                            last,
                            -(float)(ext_perimeter_width / 2 + (1 - thin_perimeter) * ext_perimeter_spacing / 2 - 1),
                            +(float)((1 - thin_perimeter) * ext_perimeter_spacing / 2 - 1),
                            ClipperLib::JoinType::jtMiter,
                            3);
                    } else {
                        next_onion = offset2_ex(
                            last,
                            -(float)(ext_perimeter_width / 2 + ext_perimeter_spacing / 2 - 1),
                            +(float)(ext_perimeter_spacing / 2 + 1),
                            ClipperLib::JoinType::jtMiter,
                            3);
                    }
                    if (thin_perimeter < 0.7) {
                        //offset2_ex can create artifacts, if too big. see superslicer#2428
                        next_onion = intersection_ex(next_onion,
                            offset_ex(
                                last,
                                -(float)(ext_perimeter_width / 2),
                                ClipperLib::JoinType::jtMiter,
                                3));
                    }


                    // look for thin walls
                    if (this->config->thin_walls) {
                        // detect edge case where a curve can be split in multiple small chunks.
                        if (allow_perimeter_anti_hysteresis) {
                            std::vector<float> divs = { 2.1f, 1.9f, 2.2f, 1.75f, 1.5f }; //don't go too far, it's not possible to print thin wall after that
                            size_t idx_div = 0;
                            while (next_onion.size() > last.size() && idx_div < divs.size()) {
                                float div = divs[idx_div];
                                //use a sightly bigger spacing to try to drastically improve the split, that can lead to very thick gapfill
                                ExPolygons next_onion_secondTry = offset2_ex(
                                    last,
                                    -(float)((ext_perimeter_width / 2) + (ext_perimeter_spacing / div) - 1),
                                    +(float)((ext_perimeter_spacing / div) - 1));
                                if (next_onion.size() > next_onion_secondTry.size() * 1.2 && next_onion.size() > next_onion_secondTry.size() + 2) {
                                    next_onion = next_onion_secondTry;
                                }
                                idx_div++;
                            }
                        }

                        // the following offset2 ensures almost nothing in @thin_walls is narrower than $min_width
                        // (actually, something larger than that still may exist due to mitering or other causes)
                        coord_t min_width = scale_t(this->config->thin_walls_min_width.get_abs_value(this->ext_perimeter_flow.nozzle_diameter()));

                        ExPolygons no_thin_zone = offset_ex(next_onion, double(ext_perimeter_width / 2), jtSquare);
                        // medial axis requires non-overlapping geometry
                        ExPolygons thin_zones = diff_ex(last, no_thin_zone, ApplySafetyOffset::Yes);
                        //don't use offset2_ex, because we don't want to merge the zones that have been separated.
                            //a very little bit of overlap can be created here with other thin polygons, but it's more useful than worisome.
                        ExPolygons half_thins = offset_ex(thin_zones, double(-min_width / 2));
                        //simplify them
                        for (ExPolygon& half_thin : half_thins) {
                            half_thin.remove_point_too_near(ext_perimeter_width/20);
                        }
                        //we push the bits removed and put them into what we will use as our anchor
                        if (half_thins.size() > 0) {
                            no_thin_zone = diff_ex(last, offset_ex(half_thins, double(min_width / 2 - SCALED_EPSILON)), ApplySafetyOffset::Yes);
                        }
                        ExPolygons thins;
                        // compute a bit of overlap to anchor thin walls inside the print.
                        for (ExPolygon& half_thin : half_thins) {
                            //growing back the polygon
                            ExPolygons thin = offset_ex(half_thin, double(min_width / 2));
                            assert(thin.size() <= 1);
                            if (thin.empty()) continue;
                            coord_t thin_walls_overlap = scale_t(this->config->thin_walls_overlap.get_abs_value(this->ext_perimeter_flow.nozzle_diameter()));
                            ExPolygons anchor = intersection_ex(offset_ex(half_thin, double(min_width / 2) +
                                (float)(thin_walls_overlap), jtSquare), no_thin_zone, ApplySafetyOffset::Yes);
                            ExPolygons bounds = union_ex(thin, anchor, ApplySafetyOffset::Yes);
                            for (ExPolygon& bound : bounds) {
                                if (!intersection_ex(thin[0], bound).empty()) {
                                    //be sure it's not too small to extrude reliably
                                    thin[0].remove_point_too_near(ext_perimeter_width / 10);
                                    if (thin[0].area() > min_width * (ext_perimeter_width + ext_perimeter_spacing)) {
                                        thins.push_back(thin[0]);
                                        bound.remove_point_too_near(ext_perimeter_width / 10);
                                        // the maximum thickness of our thin wall area is equal to the minimum thickness of a single loop (*1.2 because of circles approx. and enlrgment from 'div')
                                        Slic3r::Geometry::MedialAxis ma{ thin[0], (coord_t)((ext_perimeter_width + ext_perimeter_spacing) * 1.2),
                                            min_width, coord_t(this->layer->height) };
                                        ma.use_bounds(bound)
                                            .use_min_real_width(scale_t(this->ext_perimeter_flow.nozzle_diameter()))
                                            .use_tapers(thin_walls_overlap)
                                            .set_min_length(ext_perimeter_width + ext_perimeter_spacing)
                                            .build(thin_walls_thickpolys);
                                    }
                                    break;
                                }
                            }
                        }
                        // use perimeters to extrude area that can't be printed by thin walls
                        // it's a bit like re-add thin area into perimeter area.
                        // it can over-extrude a bit, but it's for a better good.
                        {
                            if (thin_perimeter > 0.98)
                                next_onion = union_ex(next_onion, offset_ex(diff_ex(last, thins, ApplySafetyOffset::Yes),
                                    -(float)(ext_perimeter_width / 2),
                                    ClipperLib::JoinType::jtMiter,
                                    3));
                            else if (thin_perimeter > 0.01)
                                next_onion = union_ex(next_onion, offset2_ex(diff_ex(last, thins, ApplySafetyOffset::Yes),
                                    -(float)((ext_perimeter_width / 2) + ((1 - thin_perimeter) * ext_perimeter_spacing / 4)),
                                    (float)((1 - thin_perimeter) * ext_perimeter_spacing / 4),
                                    ClipperLib::JoinType::jtMiter,
                                    3));
                            else
                                next_onion = union_ex(next_onion, offset2_ex(diff_ex(last, thins, ApplySafetyOffset::Yes),
                                    -(float)((ext_perimeter_width / 2) + (ext_perimeter_spacing / 4)),
                                    (float)(ext_perimeter_spacing / 4),
                                    ClipperLib::JoinType::jtMiter,
                                    3));
                            //simplify the loop to avoid almost-0 segments
                            resolution = get_resolution(1, false, &surface);
                            ExPolygons next_onion_temp;
                            for (ExPolygon& exp : next_onion)
                                exp.simplify((resolution < SCALED_EPSILON ? SCALED_EPSILON : resolution), &next_onion_temp);
                            //mask
                            next_onion = intersection_ex(next_onion_temp, last);
                        }
                    }
                    if (m_spiral_vase && next_onion.size() > 1) {
                        // Remove all but the largest area polygon.
                        keep_largest_contour_only(next_onion);
                    }
                } else {
                    //FIXME Is this offset correct if the line width of the inner perimeters differs
                    // from the line width of the infill?
                    coord_t good_spacing = (perimeter_idx == 1) ? ext_perimeter_spacing2 : perimeter_spacing;
                    if (thin_perimeter <= 0.98) {
                        // This path will ensure, that the perimeters do not overfill, as in 
                        // prusa3d/Slic3r GH #32, but with the cost of rounding the perimeters
                        // excessively, creating gaps, which then need to be filled in by the not very 
                        // reliable gap fill algorithm.
                        // Also the offset2(perimeter, -x, x) may sometimes lead to a perimeter, which is larger than
                        // the original.
                        next_onion = offset2_ex(last,
                            -(float)(good_spacing + (1 - thin_perimeter) * perimeter_spacing / 2 - 1),
                            +(float)((1 - thin_perimeter) * perimeter_spacing / 2 - 1),
                            (round_peri ? ClipperLib::JoinType::jtRound : ClipperLib::JoinType::jtMiter),
                            (round_peri ? min_round_spacing : 3));
                        if (allow_perimeter_anti_hysteresis) {
                            // now try with different min spacing if we fear some hysteresis
                            //TODO, do that for each polygon from last, instead to do for all of them in one go.
                            ExPolygons no_thin_onion = offset_ex(last, double(-good_spacing));
                            if (last_area < 0) {
                                last_area = 0;
                                for (const ExPolygon& expoly : last) {
                                    last_area += expoly.area();
                                }
                            }
                            double new_area = 0;
                            for (const ExPolygon& expoly : next_onion) {
                                new_area += expoly.area();
                            }

                            std::vector<float> divs{ 1.8f, 1.6f }; //don't over-extrude, so don't use divider >2
                            size_t idx_div = 0;
                            while ((next_onion.size() > no_thin_onion.size() || (new_area != 0 && last_area > new_area * 100)) && idx_div < divs.size()) {
                                float div = divs[idx_div];
                                //use a sightly bigger spacing to try to drastically improve the split, that can lead to very thick gapfill
                                ExPolygons next_onion_secondTry = offset2_ex(
                                    last,
                                    -(float)(good_spacing + (1 - thin_perimeter) * (perimeter_spacing / div) - 1),
                                    +(float)((1 - thin_perimeter) * (perimeter_spacing / div) - 1));
                                if (next_onion.size() > next_onion_secondTry.size() * 1.2 && next_onion.size() > next_onion_secondTry.size() + 2) {
                                    // don't get it if it creates too many
                                    next_onion = next_onion_secondTry;
                                } else if (next_onion.size() > next_onion_secondTry.size() || last_area > new_area * 100) {
                                    // don't get it if it's too small
                                    double area_new = 0;
                                    for (const ExPolygon& expoly : next_onion_secondTry) {
                                        area_new += expoly.area();
                                    }
                                    if (last_area > area_new * 100 || new_area == 0) {
                                        next_onion = next_onion_secondTry;
                                    }
                                }
                                idx_div++;
                            }
                            last_area = new_area;
                        }
                    } else {
                        // If "overlapping_perimeters" is enabled, this paths will be entered, which 
                        // leads to overflows, as in prusa3d/Slic3r GH #32
                        next_onion = offset_ex(last, double(-good_spacing),
                            (round_peri ? ClipperLib::JoinType::jtRound : ClipperLib::JoinType::jtMiter),
                            (round_peri ? min_round_spacing : 3));
                    }
                    // look for gaps
                    if (this->config->gap_fill_enabled.value
                        //check if we are going to have an other perimeter
                        && (perimeter_idx <= loop_number || has_overhang || next_onion.empty() || (this->config->gap_fill_last.value && perimeter_idx == loop_number + 1))) {
                        // not using safety offset here would "detect" very narrow gaps
                        // (but still long enough to escape the area threshold) that gap fill
                        // won't be able to fill but we'd still remove from infill area
                        no_last_gapfill = offset_ex(next_onion, 0.5f * good_spacing + 10,
                            (round_peri ? ClipperLib::JoinType::jtRound : ClipperLib::JoinType::jtMiter),
                            (round_peri ? min_round_spacing : 3));
                        if (perimeter_idx == 1) {
                            append(gaps, diff_ex(
                                offset_ex(last, -0.5f * gap_fill_spacing_external),
                                no_last_gapfill));  // safety offset
                        } else {
                            append(gaps, diff_ex(
                                offset_ex(last, -0.5f * gap_fill_spacing),
                                no_last_gapfill));  // safety offset
                        }
                    }
                }
                //{
                //    static int aodfjiaz = 0;
                //    std::stringstream stri;
                //    stri << this->layer->id() << "_perimeter_loop_" << (aodfjiaz++) << ".svg";
                //    SVG svg(stri.str());
                //    svg.draw(surface.expolygon, "grey");
                //    svg.draw(to_polylines(last), "yellow");
                //    svg.draw(to_polylines(next_onion), "green");
                //    svg.Close();
                //}

                if (next_onion.empty()) {
                    // Store the number of loops actually generated.
                    loop_number = perimeter_idx - 1;
                    // No region left to be filled in.
                    last.clear();
                    break;
                } else if (perimeter_idx > loop_number) {
                    if (has_overhang) {
                        loop_number++;
                        contours.emplace_back();
                        holes.emplace_back();
                    } else {
                        // If perimeter_idx > loop_number, we were looking just for gaps.
                        break;
                    }
                }

                // fuzzify
                const bool fuzzify_contours = this->config->fuzzy_skin != FuzzySkinType::None && perimeter_idx == 0 && this->layer->id() > 0;
                const bool fuzzify_holes = this->layer->id() > 0 && this->config->fuzzy_skin == FuzzySkinType::All;
                for (const ExPolygon& expolygon : next_onion) {
                    //TODO: add width here to allow variable width (if we want to extrude a sightly bigger perimeter, see thin wall)
                    contours[perimeter_idx].emplace_back(expolygon.contour, perimeter_idx, true, has_steep_overhang, fuzzify_contours || fuzzify_holes);
                    if (!expolygon.holes.empty()) {
                        holes[perimeter_idx].reserve(holes[perimeter_idx].size() + expolygon.holes.size());
                        for (const Polygon& hole : expolygon.holes)
                            holes[perimeter_idx].emplace_back(hole, perimeter_idx, false, has_steep_overhang, fuzzify_holes);
                    }
                }

                //simplify the loop to avoid artifacts when shrinking almost-0 segments
                resolution = get_resolution(perimeter_idx + 1, false, &surface);
                last.clear();
                for(ExPolygon& exp : next_onion)
                    exp.simplify((resolution < SCALED_EPSILON ? SCALED_EPSILON : resolution), &last);

                //store surface for top infill if only_one_perimeter_top
                if (perimeter_idx == 0 && (config->only_one_perimeter_top && this->upper_slices != NULL)) {
                    if (this->config->only_one_perimeter_top_other_algo) {
                        //split the polygons with top/not_top
                        //get the offset from solid surface anchor
                        coord_t offset_top_surface = scale_(config->external_infill_margin.get_abs_value(
                            config->perimeters.value == 0 ? 0. : unscaled(double(ext_perimeter_width + perimeter_spacing * int(int(config->perimeters.value) - int(1))))));
                        // if possible, try to not push the extra perimeters inside the sparse infill
                        if (offset_top_surface > 0.9 * (config->perimeters.value <= 1 ? 0. : (perimeter_spacing * (config->perimeters.value - 1))))
                            offset_top_surface -= coord_t(0.9 * (config->perimeters.value <= 1 ? 0. : (perimeter_spacing * (config->perimeters.value - 1))));
                        else offset_top_surface = 0;
                        //don't takes into account too thin areas
                        double min_width_top_surface = std::max(double(ext_perimeter_spacing / 2 + 10), scale_d(this->config->min_width_top_surface.get_abs_value(unscaled(perimeter_width))));
                        //make thin upper surfaces disapear with -+offset_top_surface
                        ExPolygons grown_upper_slices;
                        //do offset2 per island, to avoid big blob merging
                        //remove polygon too thin (but don't mess with holes)
                        for (const ExPolygon& expoly_to_grow : *this->upper_slices) {
                            //only offset the contour, as it can merge holes
                            Polygons contour = offset2(ExPolygons{ ExPolygon{expoly_to_grow.contour} }, -offset_top_surface, offset_top_surface + min_width_top_surface + (have_to_grow_for_miller ? (double)mill_extra_size : 0));
                            if (!contour.empty()) {
                                if (expoly_to_grow.holes.empty()) {
                                    for (Polygon& p : contour)
                                        grown_upper_slices.push_back(ExPolygon{ p });
                                } else {
                                    Polygons holes = expoly_to_grow.holes;
                                    for (Polygon& h : holes)
                                        h.reverse();
                                    holes = offset(holes, -min_width_top_surface - (have_to_grow_for_miller ? (double)mill_extra_size : 0));
                                    for (ExPolygon p : diff_ex(contour, holes))
                                        grown_upper_slices.push_back(p);
                                }
                            }
                        }
                        grown_upper_slices = union_ex(grown_upper_slices);
                        //set the clip to a virtual "second perimeter"
                        fill_clip = offset_ex(last, -double(ext_perimeter_spacing));
                        auto fill_clip_old = fill_clip;
                        // get the real top surface
                        const ExPolygons top_grown_polygons = (!have_to_grow_for_miller)
                            ? diff_ex(last, grown_upper_slices, ApplySafetyOffset::Yes)
                            : (unmillable.empty())
                            ? diff_ex(last, grown_upper_slices, ApplySafetyOffset::Yes)
                            : diff_ex(last, diff_ex(grown_upper_slices, unmillable, ApplySafetyOffset::Yes));

                        //get the not-top surface, from the "real top" but enlarged by external_infill_margin (and the min_width_top_surface we removed a bit before)
                        const ExPolygons inner_polygons = diff_ex(last,
                            offset_ex(top_grown_polygons, offset_top_surface + min_width_top_surface
                                //also remove the ext_perimeter_spacing/2 width because we are faking the external periemter, and we will remove ext_perimeter_spacing2
                                - double(ext_perimeter_spacing / 2)), ApplySafetyOffset::Yes);
                        // get the enlarged top surface, by using inner_polygons instead of upper_slices, and clip it for it to be exactly the polygons to fill.
                        const ExPolygons top_polygons = diff_ex(fill_clip, inner_polygons, ApplySafetyOffset::Yes);
                        // increase by half peri the inner space to fill the frontier between last and stored.
                        top_fills = union_ex(top_fills, top_polygons);
                        //set the clip to the external wall but go back inside by infill_extrusion_width/2 to be sure the extrusion won't go outside even with a 100% overlap.
                        double infill_spacing_unscaled = this->config->infill_extrusion_width.get_abs_value(this->solid_infill_flow.nozzle_diameter());
                        if (infill_spacing_unscaled == 0) infill_spacing_unscaled = Flow::auto_extrusion_width(frInfill, this->solid_infill_flow.nozzle_diameter());
                        fill_clip = offset_ex(last, double(ext_perimeter_spacing / 2) - scale_d(infill_spacing_unscaled / 2));
                        last = intersection_ex(inner_polygons, last);
                        //{
                        //    static int isazfn = 0;
                        //    std::stringstream stri;
                        //    stri << this->layer->id() << "_" << perimeter_idx << "_"<< isazfn++ <<"_only_one_peri"<< ".svg";
                        //    SVG svg(stri.str());
                        //    svg.draw(to_polylines(oldLast), "orange");
                        //    svg.draw(to_polylines(fill_clip), "purple");
                        //    svg.draw(to_polylines(inner_polygons), "yellow");
                        //    svg.draw(to_polylines(top_polygons), "cyan");
                        //    svg.draw(to_polylines(last), "red");
                        //    svg.draw(to_polylines(fill_clip_old), "green");
                        //    svg.Close();
                        //}
                    } else {

                        //split the polygons with top/not_top
                        //get the offset from solid surface anchor
                        coord_t offset_top_surface = scale_(config->external_infill_margin.get_abs_value(
                            config->perimeters.value == 0 ? 0. : unscaled(double(ext_perimeter_width + perimeter_spacing * int(int(config->perimeters.value) - int(1))))));
                        // if possible, try to not push the extra perimeters inside the sparse infill
                        if (offset_top_surface > 0.9 * (config->perimeters.value <= 1 ? 0. : (perimeter_spacing * (config->perimeters.value - 1))))
                            offset_top_surface -= coord_t(0.9 * (config->perimeters.value <= 1 ? 0. : (perimeter_spacing * (config->perimeters.value - 1))));
                        else offset_top_surface = 0;
                        //don't takes into account too thin areas
                        double min_width_top_surface = std::max(double(ext_perimeter_spacing / 2 + 10), scale_d(this->config->min_width_top_surface.get_abs_value(unscaled(perimeter_width))));
                        ExPolygons grown_upper_slices = offset_ex(*this->upper_slices, min_width_top_surface);
                        //set the clip to a virtual "second perimeter"
                        fill_clip = offset_ex(last, -double(ext_perimeter_spacing));
                        // get the real top surface
                        ExPolygons top_polygons = (!have_to_grow_for_miller)
                            ? diff_ex(last, grown_upper_slices, ApplySafetyOffset::Yes)
                            : (unmillable.empty())
                            ? diff_ex(last, offset_ex(grown_upper_slices, (double)mill_extra_size), ApplySafetyOffset::Yes)
                            : diff_ex(last, diff_ex(offset_ex(grown_upper_slices, (double)mill_extra_size), unmillable, ApplySafetyOffset::Yes));


                        //get the not-top surface, from the "real top" but enlarged by external_infill_margin (and the min_width_top_surface we removed a bit before)
                        ExPolygons inner_polygons = diff_ex(last, offset_ex(top_polygons, offset_top_surface + min_width_top_surface
                            //also remove the ext_perimeter_spacing/2 width because we are faking the external perimeter, and we will remove ext_perimeter_spacing2
                            - double(ext_perimeter_spacing / 2)), ApplySafetyOffset::Yes);
                        // get the enlarged top surface, by using inner_polygons instead of upper_slices, and clip it for it to be exactly the polygons to fill.
                        top_polygons = diff_ex(fill_clip, inner_polygons, ApplySafetyOffset::Yes);
                        // increase by half peri the inner space to fill the frontier between last and stored.
                        top_fills = union_ex(top_fills, top_polygons);
                        //set the clip to the external wall but go back inside by infill_extrusion_width/2 to be sure the extrusion won't go outside even with a 100% overlap.
                        fill_clip = offset_ex(last, double(ext_perimeter_spacing / 2) - this->config->infill_extrusion_width.get_abs_value(this->solid_infill_flow.nozzle_diameter()) / 2);
                        //ExPolygons oldLast = last;
                        last = intersection_ex(inner_polygons, last);
                        //{
                        //    std::stringstream stri;
                        //    stri << this->layer->id() << "_1_"<< perimeter_idx <<"_only_one_peri"<< ".svg";
                        //    SVG svg(stri.str());
                        //    svg.draw(to_polylines(top_fills), "green");
                        //    svg.draw(to_polylines(inner_polygons), "yellow");
                        //    svg.draw(to_polylines(top_polygons), "cyan");
                        //    svg.draw(to_polylines(oldLast), "orange");
                        //    svg.draw(to_polylines(last), "red");
                        //    svg.Close();
                        //}
                    }
                }
            }

            // fuzzify
            const bool fuzzify_gapfill = this->config->fuzzy_skin == FuzzySkinType::All && this->layer->id() > 0;
            // check for extracting extra perimeters from gapfill
            if (!gaps.empty()) {
                // if needed, add it to the first empty contour list
                const size_t contours_size = loop_number + 1;
                //first, find loops and try to extract a perimeter from them.
                for (size_t gap_idx = 0; gap_idx < gaps.size(); gap_idx++) {
                    ExPolygon& expoly = gaps[gap_idx];
                    if (!expoly.holes.empty()) {
                        //this is a a sort of a loop
                        //try to see if it's possible to add a "perimeter"
                        ExPolygons contour_expolygon = offset_ex(expoly, -(float)(perimeter_spacing / 2), ClipperLib::jtMiter, 3);
                        if (contour_expolygon.size() == 1 && !contour_expolygon.front().holes.empty()) {
                            //OK
                            // update list & variable to let the new perimeter be taken into account
                            loop_number = contours_size;
                            if (contours_size >= contours.size()) {
                                contours.emplace_back();
                                holes.emplace_back();
                            }
                        //Add the new perimeter
                            contours[contours_size].emplace_back(contour_expolygon.front().contour, contours_size, true, has_steep_overhang, fuzzify_gapfill);
                            //create the new gapfills
                            ExPolygons gapfill_area = offset_ex(Polygons{ expoly.contour }, -(float)(perimeter_spacing));
                            ExPolygons to_add = intersection_ex(ExPolygons{ expoly }, gapfill_area);
                            //add the new gapfill
                            if (to_add.size() == 0)
                                expoly.clear();
                            else
                                expoly = to_add.front();
                            for (size_t j = 1; j < to_add.size(); j++)
                                gaps.emplace_back(to_add[j]);
                        }
                    }
                }
            }

            // nest loops: holes first
            for (int d = 0; d <= loop_number; ++d) {
                PerimeterGeneratorLoops& holes_d = holes[d];
                // loop through all holes having depth == d
                for (int hole_idx = 0; hole_idx < (int)holes_d.size(); ++hole_idx) {
                    const PerimeterGeneratorLoop& loop = holes_d[hole_idx];
                    // find the hole loop that contains this one, if any
                    for (int t = d + 1; t <= loop_number; ++t) {
                        for (int j = 0; j < (int)holes[t].size(); ++j) {
                            PerimeterGeneratorLoop& candidate_parent = holes[t][j];
                            if (candidate_parent.polygon.contains(loop.polygon.first_point())) {
                                candidate_parent.children.push_back(loop);
                                holes_d.erase(holes_d.begin() + hole_idx);
                                --hole_idx;
                                goto NEXT_LOOP;
                            }
                        }
                    }
                    // if no hole contains this hole, find the contour loop that contains it
                    for (int t = loop_number; t >= 0; --t) {
                        for (int j = 0; j < (int)contours[t].size(); ++j) {
                            PerimeterGeneratorLoop& candidate_parent = contours[t][j];
                            if (candidate_parent.polygon.contains(loop.polygon.first_point())) {
                                candidate_parent.children.push_back(loop);
                                holes_d.erase(holes_d.begin() + hole_idx);
                                --hole_idx;
                                goto NEXT_LOOP;
                            }
                        }
                    }
                NEXT_LOOP:;
                }
            }
            // nest contour loops
            for (int d = loop_number; d >= 1; --d) {
                PerimeterGeneratorLoops& contours_d = contours[d];
                // loop through all contours having depth == d
                for (int contour_idx = 0; contour_idx < (int)contours_d.size(); ++contour_idx) {
                    const PerimeterGeneratorLoop& loop = contours_d[contour_idx];
                    // find the contour loop that contains it
                    for (int t = d - 1; t >= 0; --t) {
                        for (size_t j = 0; j < contours[t].size(); ++j) {
                            PerimeterGeneratorLoop& candidate_parent = contours[t][j];
                            if (candidate_parent.polygon.contains(loop.polygon.first_point())) {
                                candidate_parent.children.push_back(loop);
                                contours_d.erase(contours_d.begin() + contour_idx);
                                --contour_idx;
                                goto NEXT_CONTOUR;
                            }
                        }
                    }
                NEXT_CONTOUR:;
                }
            }
            // at this point, all loops should be in contours[0] (= contours.front() )
            // collection of loops to add into loops
            ExtrusionEntityCollection peri_entities;
            if (config->perimeter_loop.value) {
                //onlyone_perimeter = >fusion all perimeterLoops
                for (PerimeterGeneratorLoop& loop : contours.front()) {
                    ExtrusionLoop extr_loop = this->_traverse_and_join_loops(loop, get_all_Childs(loop), loop.polygon.points.front());
                    //ExtrusionLoop extr_loop = this->_traverse_and_join_loops_old(loop, loop.polygon.points.front(), true);
                    extr_loop.paths.back().polyline.points.push_back(extr_loop.paths.front().polyline.points.front());
                    peri_entities.append(extr_loop);
                }

                // append thin walls
                if (!thin_walls_thickpolys.empty()) {
                    if (this->object_config->thin_walls_merge) {
                        _merge_thin_walls(peri_entities, thin_walls_thickpolys);
                    } else {
                        peri_entities.append(Geometry::thin_variable_width(
                            thin_walls_thickpolys, 
                            erThinWall, 
                            this->ext_perimeter_flow, 
                            std::max(ext_perimeter_width / 4, scale_t(this->print_config->resolution))));
                    }
                    thin_walls_thickpolys.clear();
                }
            } else {
                if (this->object_config->thin_walls_merge) {
                    ThickPolylines no_thin_walls;
                    peri_entities = this->_traverse_loops(contours.front(), no_thin_walls);
                    _merge_thin_walls(peri_entities, thin_walls_thickpolys);
                } else {
                    peri_entities = this->_traverse_loops(contours.front(), thin_walls_thickpolys);
                }
            }
#if _DEBUG
        peri_entities.visit(LoopAssertVisitor{});
#endif
            //{
            //    static int aodfjiaqsdz = 0;
            //    std::stringstream stri;
            //    stri << this->layer->id() << "_perimeter_loops_" << (aodfjiaqsdz++) << ".svg";
            //    SVG svg(stri.str());
            //    svg.draw(surface.expolygon, "grey");
            //    struct TempVisitor : public ExtrusionVisitorRecursiveConst {
            //        SVG* svg;
            //        virtual void use(const ExtrusionPath& path) override { svg->draw(path.polyline, "green"); }
            //    } bbvisitor;
            //    bbvisitor.svg = &svg;
            //    peri_entities.visit(bbvisitor);
            //    svg.Close();
            //}


            // if brim will be printed, reverse the order of perimeters so that
            // we continue inwards after having finished the brim
            // be careful to not print thin walls before perimeters (gapfill will be added after so don't worry for them)
            // TODO: add test for perimeter order
            const bool brim_first_layer = this->layer->id() == 0 && (this->object_config->brim_width.value > 0 || this->object_config->brim_width_interior.value > 0);
            if (this->config->external_perimeters_first || brim_first_layer) {
                if (this->config->external_perimeters_nothole.value || brim_first_layer) {
                    if (this->config->external_perimeters_hole.value || brim_first_layer) {
                        //reverse only not-thin wall
                        ExtrusionEntityCollection coll2;
                        for (const ExtrusionEntity* loop : peri_entities.entities()) {
                            if ( (loop->is_loop() && loop->role() != erThinWall)) {
                                coll2.append(*loop);
                            }
                        }
                        coll2.reverse();
                        for (const ExtrusionEntity* loop : peri_entities.entities()) {
                            if (!((loop->is_loop() && loop->role() != erThinWall))) {
                                coll2.append(*loop);
                            }
                        }
                        //note: this hacky thing is possible because coll2.entities contains in fact peri_entities's entities
                        //if you does peri_entities = coll2, you'll delete peri_entities's entities() and then you have nothing.
                        peri_entities = std::move(coll2);
                    } else {
                        //reverse only not-hole perimeters
                        ExtrusionEntityCollection coll2;
                        for (const ExtrusionEntity* loop : peri_entities.entities()) {
                            if ((loop->is_loop() && loop->role() != erThinWall) && !(((ExtrusionLoop*)loop)->loop_role() & ExtrusionLoopRole::elrHole) != 0) {
                                coll2.append(*loop);
                            }
                        }
                        coll2.reverse();
                        for (const ExtrusionEntity* loop : peri_entities.entities()) {
                            if (!((loop->is_loop() && loop->role() != erThinWall) && !(((ExtrusionLoop*)loop)->loop_role() & ExtrusionLoopRole::elrHole) != 0)) {
                                coll2.append(*loop);
                            }
                        }
                        //note: this hacky thing is possible because coll2.entities contains in fact entities's entities
                        //if you does peri_entities = coll2, you'll delete peri_entities's entities and then you have nothing.
                        peri_entities = std::move(coll2);
                    }
                } else if (this->config->external_perimeters_hole.value) {
                    //reverse the hole, and put them in first place.
                    ExtrusionEntityCollection coll2;
                    for (const ExtrusionEntity* loop : peri_entities.entities()) {
                        if ((loop->is_loop() && loop->role() != erThinWall) && (((ExtrusionLoop*)loop)->loop_role() & ExtrusionLoopRole::elrHole) != 0) {
                            coll2.append(*loop);
                        }
                    }
                    coll2.reverse();
                    for (const ExtrusionEntity* loop : peri_entities.entities()) {
                        if (!((loop->is_loop() && loop->role() != erThinWall) && (((ExtrusionLoop*)loop)->loop_role() & ExtrusionLoopRole::elrHole) != 0)) {
                            coll2.append(*loop);
                        }
                    }
                    //note: this hacky thing is possible because coll2.entities contains in fact peri_entities's entities
                    //if you does peri_entities = coll2, you'll delete peri_entities's entities and then you have nothing.
                    peri_entities = std::move(coll2);
                }

            }
            // append perimeters for this slice as a collection
            if (!peri_entities.empty()) {
                //move it, to avoid to clone evrything and then delete it
                this->loops->append(peri_entities);
            }
        } // for each loop of an island
#if _DEBUG
    this->loops->visit(LoopAssertVisitor{});
#endif

        // fill gaps
        ExPolygons gaps_ex;
        ExPolygons gap_srf;
        if (!gaps.empty()) {
            // collapse 
            double min = 0.2 * perimeter_width * (1 - INSET_OVERLAP_TOLERANCE);
            //be sure we don't gapfill where the perimeters are already touching each other (negative spacing).
            min = std::max(min, double(Flow::new_from_spacing((float)EPSILON, (float)this->perimeter_flow.nozzle_diameter(), (float)this->layer->height, (float)this->perimeter_flow.spacing_ratio(), false).scaled_width()));
            double max = 2.2 * perimeter_spacing;
            //remove areas that are too big (shouldn't occur...)
            ExPolygons too_big = offset2_ex(gaps, double(-max / 2), double(+max / 2));
            ExPolygons gaps_ex_to_test = too_big.empty() ? gaps : diff_ex(gaps, too_big, ApplySafetyOffset::Yes);
            const double minarea = scale_(scale_(this->config->gap_fill_min_area.get_abs_value(unscaled((double)perimeter_width) * unscaled((double)perimeter_width))));
            // check each gapfill area to see if it's printable.
            for (const ExPolygon& expoly : gaps_ex_to_test) {
                //remove too small gaps that are too hard to fill.
                //ie one that are smaller than an extrusion with width of min and a length of max.
                if (expoly.area() > minarea) {
                    ExPolygons expoly_after_shrink_test = offset_ex(ExPolygons{ expoly }, double(-min * 0.5));
                    //if the shrink split the area in multipe bits
                    if (expoly_after_shrink_test.size() > 1) {
                        //remove too small bits
                        for (int exp_idx = 0; exp_idx < expoly_after_shrink_test.size(); exp_idx++) {
                            if (expoly_after_shrink_test[exp_idx].area() < (SCALED_EPSILON * SCALED_EPSILON * 4)) {
                                expoly_after_shrink_test.erase(expoly_after_shrink_test.begin() + exp_idx);
                                exp_idx--;
                            } else {
                                ExPolygons wider = offset_ex(ExPolygons{ expoly_after_shrink_test[exp_idx] }, min * 0.5);
                                if (wider.empty() || wider[0].area() < minarea) {
                                    expoly_after_shrink_test.erase(expoly_after_shrink_test.begin() + exp_idx);
                                    exp_idx--;
                                }
                            }
                        }
                        //maybe some areas are a just bit too thin, try with just a little more offset to remove them.
                        ExPolygons expoly_after_shrink_test2 = offset_ex(ExPolygons{ expoly }, double(-min * 0.8));
                        for (int exp_idx = 0; exp_idx < expoly_after_shrink_test2.size(); exp_idx++) {
                            if (expoly_after_shrink_test2[exp_idx].area() < (SCALED_EPSILON * SCALED_EPSILON * 4)) {
                                expoly_after_shrink_test2.erase(expoly_after_shrink_test2.begin() + exp_idx);
                                exp_idx--;

                            } else {
                                ExPolygons wider = offset_ex(ExPolygons{ expoly_after_shrink_test2[exp_idx] }, min * 0.5);
                                if (wider.empty() || wider[0].area() < minarea) {
                                    expoly_after_shrink_test2.erase(expoly_after_shrink_test2.begin() + exp_idx);
                                    exp_idx--;
                                }
                            }
                        }
                        //it's better if there are significantly less extrusions
                        if (expoly_after_shrink_test.size() / 1.42 > expoly_after_shrink_test2.size()) {
                            expoly_after_shrink_test2 = offset_ex(expoly_after_shrink_test2, double(min * 0.8));
                            //insert with move instead of copy
                            std::move(expoly_after_shrink_test2.begin(), expoly_after_shrink_test2.end(), std::back_inserter(gaps_ex));
                        } else {
                            expoly_after_shrink_test = offset_ex(expoly_after_shrink_test, double(min * 0.8));
                            std::move(expoly_after_shrink_test.begin(), expoly_after_shrink_test.end(), std::back_inserter(gaps_ex));
                        }
                    } else {
                        expoly_after_shrink_test = offset_ex(expoly_after_shrink_test, double(min * 0.8));
                        std::move(expoly_after_shrink_test.begin(), expoly_after_shrink_test.end(), std::back_inserter(gaps_ex));
                    }
                }
            }
            // create lines from the area
            ThickPolylines polylines;
            for (const ExPolygon& ex : gaps_ex) {
                Geometry::MedialAxis{ ex, coord_t(max * 1.1), coord_t(min), coord_t(this->layer->height) }.build(polylines);
            }
            // create extrusion from lines
            if (!polylines.empty()) {
                this->gap_fill->append(Geometry::thin_variable_width(
                    polylines,
                    erGapFill, 
                    this->solid_infill_flow, 
                    scale_t(this->print_config->resolution_internal)));
                /*  Make sure we don't infill narrow parts that are already gap-filled
                    (we only consider this surface's gaps to reduce the diff() complexity).
                    Growing actual extrusions ensures that gaps not filled by medial axis
                    are not subtracted from fill surfaces (they might be too short gaps
                    that medial axis skips but infill might join with other infill regions
                    and use zigzag).  */
                    // get clean surface of gap
                gap_srf = union_ex(offset(this->gap_fill->polygons_covered_by_width(float(SCALED_EPSILON) / 10), float(SCALED_EPSILON / 2)));
                // intersection to ignore the bits of gapfill tha may be over infill, as it's epsilon and there may be some voids here anyway.
                gap_srf = intersection_ex(gap_srf, gaps_ex);
                // the diff(last, gap) will be done after, as we have to keep the last un-gapped to avoid unneeded gap/infill offset
            }
        }

        // create one more offset to be used as boundary for fill
        // we offset by half the perimeter spacing (to get to the actual infill boundary)
        // and then we offset back and forth by half the infill spacing to only consider the
        // non-collapsing regions
        coord_t inset =
            (loop_number < 0) ? 0 :
            (loop_number == 0) ?
            // one loop
            ext_perimeter_spacing / 2 :
            // two or more loops?
            perimeter_spacing / 2;
        // only apply infill overlap if we actually have one perimeter
        if (inset == 0) {
            infill_peri_overlap = 0;
        }

        //remove gapfill from last
        ExPolygons last_no_gaps = (gap_srf.empty()) ? last : diff_ex(last, gap_srf);

        // simplify infill contours according to resolution
        Polygons not_filled_p;
        for (ExPolygon& ex : last_no_gaps)
            ex.simplify_p(scale_t(std::max(this->print_config->resolution.value, print_config->resolution_internal/4)), &not_filled_p);
        ExPolygons not_filled_exp = union_ex(not_filled_p);
        // collapse too narrow infill areas
        coord_t min_perimeter_infill_spacing = (coord_t)(solid_infill_spacing * (1. - INSET_OVERLAP_TOLERANCE));
        ExPolygons infill_exp;
        //special branch if gap : don't inset away from gaps!
        if (gap_srf.empty()) {
            infill_exp = offset2_ex(not_filled_exp,
                double(-inset - min_perimeter_infill_spacing / 2 + infill_peri_overlap - infill_gap),
                double(min_perimeter_infill_spacing / 2));
        } else {
            //store the infill_exp but not offseted, it will be used as a clip to remove the gapfill portion
            const ExPolygons infill_exp_no_gap = offset2_ex(not_filled_exp,
                double(-inset - min_perimeter_infill_spacing / 2 + infill_peri_overlap - infill_gap),
                double(inset + min_perimeter_infill_spacing / 2 - infill_peri_overlap + infill_gap));
            //redo the same as not_filled_exp but with last instead of last_no_gaps
            not_filled_p.clear();
            for (ExPolygon& ex : last)
                ex.simplify_p(scale_t(std::max(this->print_config->resolution.value, print_config->resolution_internal/4)), &not_filled_p);
            not_filled_exp = union_ex(not_filled_p);
            infill_exp = offset2_ex(not_filled_exp,
                double(-inset - min_perimeter_infill_spacing / 2 + infill_peri_overlap - infill_gap),
                double(min_perimeter_infill_spacing / 2));
            // intersect(growth(last-gap) , last), so you have the (last - small gap) but without voids betweeng gap & last
            infill_exp = intersection_ex(infill_exp, infill_exp_no_gap);
        }

        //if any top_fills, grow them by ext_perimeter_spacing/2 to have the real un-anchored fill
        ExPolygons top_infill_exp = intersection_ex(fill_clip, offset_ex(top_fills, double(ext_perimeter_spacing / 2)));
        if (!top_fills.empty()) {
            infill_exp = union_ex(infill_exp, offset_ex(top_infill_exp, double(infill_peri_overlap)));
        }
        // append infill areas to fill_surfaces
        this->fill_surfaces->append(infill_exp, stPosInternal | stDensSparse);

        if (infill_peri_overlap != 0) {
            ExPolygons polyWithoutOverlap;
            if (min_perimeter_infill_spacing / 2 > infill_peri_overlap)
                polyWithoutOverlap = offset2_ex(
                    not_filled_exp,
                    double(-inset - infill_gap - min_perimeter_infill_spacing / 2 + infill_peri_overlap),
                    double(min_perimeter_infill_spacing / 2 - infill_peri_overlap));
            else
                polyWithoutOverlap = offset_ex(
                    not_filled_exp,
                    double(-inset - infill_gap));
            if (!top_fills.empty()) {
                polyWithoutOverlap = union_ex(polyWithoutOverlap, top_infill_exp);
            }
            this->fill_no_overlap.insert(this->fill_no_overlap.end(), polyWithoutOverlap.begin(), polyWithoutOverlap.end());
            /*{
                static int isaqsdsdfsdfqzfn = 0;
                std::stringstream stri;
                stri << this->layer->id() << "_2_end_makeperimeter_" << isaqsdsdfsdfqzfn++ << ".svg";
                SVG svg(stri.str());
                svg.draw(to_polylines(infill_exp), "blue");
                svg.draw(to_polylines(fill_no_overlap), "cyan");
                svg.draw(to_polylines(not_filled_exp), "green");
                svg.draw(to_polylines(last_no_gaps), "yellow");
                svg.draw(to_polylines(offset_ex(fill_clip, ext_perimeter_spacing / 2)), "brown");
                svg.draw(to_polylines(top_infill_exp), "orange");
                svg.Close();
            }*/
        }
    } // for each island
}


ExtrusionPaths PerimeterGenerator::create_overhangs(const Polyline& loop_polygons, ExtrusionRole role, bool is_external) const {
    ExtrusionPaths paths;
    if (this->config->overhangs_width.get_abs_value(this->overhang_flow.nozzle_diameter()) == 0 && 0 == this->config->overhangs_width_speed.get_abs_value(this->overhang_flow.nozzle_diameter())) {
        //error
        ExtrusionPath path(role);
        path.polyline = loop_polygons;
        path.mm3_per_mm = is_external ? this->ext_mm3_per_mm()           : this->mm3_per_mm();
        path.width = is_external ?      this->ext_perimeter_flow.width() : this->perimeter_flow.width();
        path.height = (float)this->layer->height;
        assert(path.mm3_per_mm == path.mm3_per_mm);
        assert(path.width == path.width);
        assert(path.height == path.height);
        paths.push_back(path);
        return paths;
    
    }
    //set the fan & speed before the flow
    Polylines ok_polylines = { loop_polygons };

    Polylines small_speed;
    Polylines big_speed;
    bool no_small_flow = _lower_slices_bridge_speed_big == _lower_slices_bridge_flow_small;
    Polylines small_flow;
    Polylines big_flow;
#ifdef _DEBUG
    for (Polyline& poly : ok_polylines)
        for (int i = 0; i < poly.points.size() - 1; i++)
            assert(poly.points[i] != poly.points[i + 1]);
#endif

    Polylines* previous = &ok_polylines;
    if (this->config->overhangs_width_speed.value > 0 && (this->config->overhangs_width_speed.value < this->config->overhangs_width.value || this->config->overhangs_width.value == 0)) {
        if (!this->_lower_slices_bridge_speed_small.empty()) {
            small_speed = diff_pl(*previous, this->_lower_slices_bridge_speed_small);
#ifdef _DEBUG
            for (Polyline& poly : small_speed) //                       assert small_speed
                for (int i = 0; i < poly.points.size() - 1; i++) //     assert small_speed
                    assert(poly.points[i] != poly.points[i + 1]); //    assert small_speed
#endif
            if (!small_speed.empty()) {
                *previous = intersection_pl(*previous, this->_lower_slices_bridge_speed_small);
#ifdef _DEBUG
                for (Polyline& poly : *previous) //                         assert previous
                    for (int i = 0; i < poly.points.size() - 1; i++) //     assert previous
                        assert(poly.points[i] != poly.points[i + 1]); //    assert previous
#endif
                previous = &small_speed;
            }
        }
        if (!this->_lower_slices_bridge_speed_big.empty()) {
            big_speed = diff_pl(*previous, this->_lower_slices_bridge_speed_big);
#ifdef _DEBUG
            for (Polyline& poly : big_speed) //                         assert big_speed
                for (int i = 0; i < poly.points.size() - 1; i++) //     assert big_speed
                    assert(poly.points[i] != poly.points[i + 1]); //    assert big_speed
#endif
            if (!big_speed.empty()) {
                *previous = intersection_pl(*previous, this->_lower_slices_bridge_speed_big);
#ifdef _DEBUG
                for (Polyline& poly : *previous) //                         assert previous
                    for (int i = 0; i < poly.points.size() - 1; i++) //     assert previous
                        assert(poly.points[i] != poly.points[i + 1]); //    assert previous
#endif
                previous = &big_speed;
            }
        }
    }
    if (this->config->overhangs_width.value > 0) {
        if (!this->_lower_slices_bridge_flow_small.empty()) {
            small_flow = diff_pl(*previous, this->_lower_slices_bridge_flow_small);
#ifdef _DEBUG
            for (Polyline& poly : small_flow) //                        assert small_flow
                for (int i = 0; i < poly.points.size() - 1; i++) //     assert small_flow
                    assert(poly.points[i] != poly.points[i + 1]); //    assert small_flow
#endif
            if (!small_flow.empty()) {
                *previous = intersection_pl(*previous, this->_lower_slices_bridge_flow_small);
#ifdef _DEBUG
                for (Polyline& poly : *previous) //                         assert previous
                    for (int i = 0; i < poly.points.size() - 1; i++) //     assert previous
                        assert(poly.points[i] != poly.points[i + 1]); //    assert previous
#endif
                previous = &small_flow;
            }
        }
        if (!this->_lower_slices_bridge_flow_big.empty()) {
            big_flow = diff_pl(*previous, this->_lower_slices_bridge_flow_big);
#ifdef _DEBUG
            for (Polyline& poly : big_flow) //                          assert big_flow
                for (int i = 0; i < poly.points.size() - 1; i++) //     assert big_flow
                    assert(poly.points[i] != poly.points[i + 1]); //    assert big_flow
#endif
            if (!big_flow.empty()) {
                *previous = intersection_pl(*previous, this->_lower_slices_bridge_flow_big);
#ifdef _DEBUG
                for (Polyline& poly : *previous) //                         assert previous
                    for (int i = 0; i < poly.points.size() - 1; i++) //     assert previous
                        assert(poly.points[i] != poly.points[i + 1]); //    assert previous
#endif
                previous = &big_flow;
            }
        }
    }

    //note: layer height is used to identify the path type
    if (!ok_polylines.empty()) {
        extrusion_paths_append(
            paths,
            ok_polylines,
            role,
            is_external ? this->ext_mm3_per_mm() : this->mm3_per_mm(),
            is_external ? this->ext_perimeter_flow.width() : this->perimeter_flow.width(),
            0);
    }
    if (!small_speed.empty()) {
        extrusion_paths_append(
            paths,
            small_speed,
            erOverhangPerimeter,
            is_external ? this->ext_mm3_per_mm() : this->mm3_per_mm(),
            is_external ? this->ext_perimeter_flow.width() : this->perimeter_flow.width(),
            no_small_flow ? 2 : 1);
    }
    if (!big_speed.empty()) {
        extrusion_paths_append(
            paths,
            big_speed,
            erOverhangPerimeter,
            is_external ? this->ext_mm3_per_mm() : this->mm3_per_mm(),
            is_external ? this->ext_perimeter_flow.width() : this->perimeter_flow.width(),
            no_small_flow ? 3 : 2);
    }
    if (!small_flow.empty()) {
        extrusion_paths_append(
            paths,
            small_flow,
            erOverhangPerimeter,
            this->mm3_per_mm_overhang(),
            this->overhang_flow.width(),
            3);
    }
    if (!big_flow.empty()) {
        extrusion_paths_append(
            paths,
            big_flow,
            erOverhangPerimeter,
            this->mm3_per_mm_overhang(),
            this->overhang_flow.width(),
            4);
    }

    // reapply the nearest point search for starting point
    // We allow polyline reversal because Clipper may have randomly reversed polylines during clipping.
    if(!paths.empty())
        chain_and_reorder_extrusion_paths(paths, &paths.front().first_point());

    bool has_normal = !ok_polylines.empty();
    bool has_speed = !small_speed.empty() || !big_speed.empty();
    bool has_flow = !small_flow.empty() || !big_flow.empty();

    std::function<void(ExtrusionPaths&, const std::function<bool(ExtrusionPath&, ExtrusionPath&, ExtrusionPath&)>&)> foreach = [](ExtrusionPaths &paths, const std::function<bool(ExtrusionPath&, ExtrusionPath&, ExtrusionPath&)>& doforeach) {
        if (paths.size() > 2)
            for (int i = 1; i < paths.size() - 1; i++) {
                if (doforeach(paths[i - 1], paths[i], paths[i + 1])) {
                    paths.erase(paths.begin() + i);
                    i--;
                    if (paths[i].height == paths[i + 1].height) {
                        paths[i].polyline.points.insert(paths[i].polyline.points.end(), paths[i + 1].polyline.points.begin() + 1, paths[i + 1].polyline.points.end());
                        paths.erase(paths.begin() + i + 1);
                    }
                }
            }
        if (paths.size() > 2)
            if (doforeach(paths[paths.size() - 2], paths.back(), paths.front())) {
                paths.erase(paths.end() - 1);
                if (paths.back().height == paths.front().height) {
                    paths.front().polyline.points.insert(paths.front().polyline.points.begin(), paths.back().polyline.points.begin(), paths.back().polyline.points.end() - 1);
                    paths.erase(paths.end() - 1);
                }
            }
        if (paths.size() > 2)
            if (doforeach(paths.back(), paths.front(), paths[1])) {
                paths.erase(paths.begin());
                if (paths.back().height == paths.front().height) {
                    paths.front().polyline.points.insert(paths.front().polyline.points.begin(), paths.back().polyline.points.begin(), paths.back().polyline.points.end() - 1);
                    paths.erase(paths.end() - 1);
                }
            }
    };

    if (paths.size() > 2) {
        double min_length = this->perimeter_flow.scaled_width() * 2;
        double ok_length = this->perimeter_flow.scaled_width() * 20;

        foreach(paths, [min_length, ok_length](ExtrusionPath& prev, ExtrusionPath& curr, ExtrusionPath& next) {
            if (curr.length() < min_length) {
                float diff_height = std::abs(prev.height - curr.height) - std::abs(next.height - curr.height);
                //have to choose the rigth path
                if (diff_height < 0 || (diff_height == 0 && prev.length() > next.length())) {
                    //merge to previous
                    assert(prev.last_point() == curr.first_point());
                    assert(curr.polyline.points.size() > 1);
                    prev.polyline.points.insert(prev.polyline.points.end(), curr.polyline.points.begin() + 1, curr.polyline.points.end());
                } else {
                    //merge to next
                    assert(curr.last_point() == next.first_point());
                    assert(curr.polyline.points.size() > 1);
                    next.polyline.points.insert(next.polyline.points.begin(), curr.polyline.points.begin(), curr.polyline.points.end() - 1);
                }
                return true;
            } else if(((int)curr.height) % 2 == 1 && curr.length() > ok_length){
                curr.height++;
                if (prev.height == curr.height) {
                    prev.polyline.points.insert(prev.polyline.points.end(), curr.polyline.points.begin() + 1, curr.polyline.points.end());
                    return true;
                } else if (next.height == curr.height) {
                    next.polyline.points.insert(next.polyline.points.begin(), curr.polyline.points.begin(), curr.polyline.points.end() - 1);
                    return true;
                }
            }
            return false;
        });

        foreach(paths, [](ExtrusionPath& prev, ExtrusionPath& curr, ExtrusionPath& next) {
            if (curr.height == 3) {
                //have to choose the rigth path
                if (prev.height == 4 || (prev.height == 2 && next.height < 2)) {
                    //merge to previous
                    assert(prev.last_point() == curr.first_point());
                    assert(curr.polyline.points.size() > 1);
                    prev.polyline.points.insert(prev.polyline.points.end(), curr.polyline.points.begin() + 1, curr.polyline.points.end());
                } else {
                    //merge to next
                    assert(curr.last_point() == next.first_point());
                    assert(curr.polyline.points.size() > 1);
                    next.polyline.points.insert(next.polyline.points.begin(), curr.polyline.points.begin(), curr.polyline.points.end() - 1);
                }
                return true;
            }
            return false;
        });
        foreach(paths, [](ExtrusionPath& prev, ExtrusionPath& curr, ExtrusionPath& next) {
            if (curr.height == 1) {
                //have to choose the rigth path
                if (prev.height == 2 || (prev.height == 0 && next.height > 2)) {
                    //merge to previous
                    assert(prev.last_point() == curr.first_point());
                    assert(curr.polyline.points.size() > 1);
                    prev.polyline.points.insert(prev.polyline.points.end(), curr.polyline.points.begin() + 1, curr.polyline.points.end());
                } else {
                    //merge to next
                    assert(curr.last_point() == next.first_point());
                    assert(curr.polyline.points.size() > 1);
                    next.polyline.points.insert(next.polyline.points.begin(), curr.polyline.points.begin(), curr.polyline.points.end() - 1);
                }
                return true;
            }
            return false;
        });
    }
    if(paths.size() == 2){
        double min_length = this->perimeter_flow.scaled_width() * 2;
        if (paths.front().length() < min_length) {
            paths.back().polyline.points.insert(paths.back().polyline.points.begin(), paths.front().polyline.points.begin(), paths.front().polyline.points.end() - 1);
            paths.erase(paths.begin());
        }else if (paths.back().length() < min_length) {
            paths.front().polyline.points.insert(paths.front().polyline.points.end(), paths.back().polyline.points.begin() + 1, paths.back().polyline.points.end());
            paths.erase(paths.begin() + 1);
        }
    }
    //set correct height
    for (ExtrusionPath& path : paths) {
        path.height = path.height < 3 ? (float)this->layer->height : this->overhang_flow.height();
    }

    return paths;
}

static void fuzzy_polygon(ExtrusionPaths& paths, coordf_t fuzzy_skin_thickness, coordf_t fuzzy_skin_point_dist)
{
    const coordf_t min_dist_between_points = fuzzy_skin_point_dist * 3. / 4.; // hardcoded: the point distance may vary between 3/4 and 5/4 the supplied value
    const coordf_t range_random_point_dist = fuzzy_skin_point_dist / 2.;
    coordf_t dist_left_over = coordf_t(rand()) * (min_dist_between_points / 2) / double(RAND_MAX); // the distance to be traversed on the line before making the first new point

    Point last_point = paths.back().last_point();
    bool is_loop = true; //always a loop currently paths.front().first_point() == last_point;
    Point p0 = last_point;
    for (ExtrusionPath &path : paths) {
        Points out;
        size_t next_idx = 0;
        if (p0 == path.polyline.points.front()) {
            next_idx = 1;
        }
        out.reserve(path.polyline.points.size());
        for (; next_idx < path.polyline.points.size(); next_idx++)
        {
            Point& p1 = path.polyline.points[next_idx];
            // 'a' is the (next) new point between p0 and p1
            Vec2d  p0p1 = (p1 - p0).cast<double>();
            coordf_t p0p1_size = p0p1.norm();
            // so that p0p1_size - dist_last_point evaulates to dist_left_over - p0p1_size
            coordf_t dist_last_point = dist_left_over + p0p1_size * 2.;
            for (coordf_t p0pa_dist = dist_left_over; p0pa_dist < p0p1_size;
                p0pa_dist += min_dist_between_points + coordf_t(rand()) * range_random_point_dist / double(RAND_MAX))
            {
                coordf_t r = coordf_t(rand()) * (fuzzy_skin_thickness * 2.) / double(RAND_MAX) - fuzzy_skin_thickness;
                out.emplace_back(p0 + (p0p1 * (p0pa_dist / p0p1_size) + perp(p0p1).cast<double>().normalized() * r).cast<coord_t>());
                dist_last_point = p0pa_dist;
            }
            dist_left_over = p0p1_size - dist_last_point;
            p0 = p1;
        }
        while (path.polyline.size() < 3) {
            size_t point_idx = path.polyline.size() - 2;
            out.emplace_back(path.polyline.points[point_idx]);
            if (point_idx == 0)
                break;
            --point_idx;
        }
        if (out.size() >= 3)
            path.polyline.points = std::move(out);
    }
    if (is_loop) {
        paths.back().polyline.points.push_back(paths.front().polyline.points.front());
    } else {
        paths.back().polyline.points.push_back(last_point);
    }
}

ExtrusionEntityCollection PerimeterGenerator::_traverse_loops(
    const PerimeterGeneratorLoops &loops, ThickPolylines &thin_walls) const
{
    // loops is an arrayref of ::Loop objects
    // turn each one into an ExtrusionLoop object
    ExtrusionEntitiesPtr coll;
    for (const PerimeterGeneratorLoop &loop : loops) {
        bool is_external = loop.is_external();
        
        ExtrusionRole role;
        ExtrusionLoopRole loop_role = ExtrusionLoopRole::elrDefault;
        role = is_external ? erExternalPerimeter : erPerimeter;
        if (loop.is_internal_contour()) {
            // Note that we set loop role to ContourInternalPerimeter
            // also when loop is both internal and external (i.e.
            // there's only one contour loop).
            loop_role = ExtrusionLoopRole::elrInternal;
        }
        if (!loop.is_contour) {
            loop_role = (ExtrusionLoopRole)(loop_role | ExtrusionLoopRole::elrHole);
        }
        if (loop.children.empty()) {
            loop_role = ExtrusionLoopRole(loop_role | ExtrusionLoopRole::elrFirstLoop);
        }
        if (this->config->external_perimeters_vase.value && this->config->external_perimeters_first.value && is_external) {
            if ((loop.is_contour && this->config->external_perimeters_nothole.value) || (!loop.is_contour && this->config->external_perimeters_hole.value)) {
                loop_role = (ExtrusionLoopRole)(loop_role | ExtrusionLoopRole::elrVase);
            }
        }
        
        // detect overhanging/bridging perimeters
        ExtrusionPaths paths;

        bool is_overhang = this->config->overhangs_width_speed.value > 0
            && this->layer->id() > object_config->raft_layers;
        if(this->object_config->support_material && this->object_config->support_material_contact_distance_type.value == zdNone)
            is_overhang = false;
        if (is_overhang) {
            paths = this->create_overhangs(loop.polygon.split_at_first_point(), role, is_external);
        } else {
            ExtrusionPath path(role);
            path.polyline   = loop.polygon.split_at_first_point();
            path.mm3_per_mm = is_external ? this->ext_mm3_per_mm()           : this->mm3_per_mm();
            path.width      = is_external ? this->ext_perimeter_flow.width() : this->perimeter_flow.width();
            path.height     = (float) this->layer->height;
            assert(path.mm3_per_mm == path.mm3_per_mm);
            assert(path.width == path.width);
            assert(path.height == path.height);
            paths.push_back(path);
        }
        if (loop.fuzzify) {
            double nozle_diameter = is_external ? this->ext_perimeter_flow.nozzle_diameter() : this->perimeter_flow.nozzle_diameter();
            double fuzzy_skin_thickness = config->fuzzy_skin_thickness.get_abs_value(nozle_diameter);
            double fuzzy_skin_point_dist = config->fuzzy_skin_point_dist.get_abs_value(nozle_diameter);
            fuzzy_polygon(paths, scale_d(fuzzy_skin_thickness), scale_d(fuzzy_skin_point_dist));
        }

        coll.push_back(new ExtrusionLoop(paths, loop_role));
    }
    
    // append thin walls to the nearest-neighbor search (only for first iteration)
    if (!thin_walls.empty()) {
        ExtrusionEntitiesPtr tw = Geometry::thin_variable_width(thin_walls, erThinWall, this->ext_perimeter_flow, std::max(ext_perimeter_flow.scaled_width() / 4, scale_t(this->print_config->resolution)));
        coll.insert(coll.end(), tw.begin(), tw.end());
        //don't add again
        thin_walls.clear();
    }
    // traverse children and build the final collection
    Point zero_point(0, 0);
    //result is  [idx, needReverse] ?
    std::vector<std::pair<size_t, bool>> chain = chain_extrusion_entities(coll, &zero_point);
    ExtrusionEntityCollection coll_out;
    if (chain.empty()) return coll_out;

    //little check: if you have external holes with only one extrusion and internal things, please draw the internal first, just in case it can help print the hole better.
    std::vector<std::pair<size_t, bool>> better_chain;
    for (const std::pair<size_t, bool>& idx : chain) {
        if(idx.first < loops.size())
            if (!loops[idx.first].is_external() || (!loops[idx.first].is_contour && !loops[idx.first].children.empty()))
                better_chain.push_back(idx);
    }
    for (const std::pair<size_t, bool>& idx : chain) {
        if (idx.first < loops.size())
            if (idx.first < loops.size() && loops[idx.first].is_external() && !(!loops[idx.first].is_contour && !loops[idx.first].children.empty()))
                better_chain.push_back(idx);
    }
    //thin walls always last!
    for (const std::pair<size_t, bool>& idx : chain) {
        if (idx.first >= loops.size())
            better_chain.push_back(idx);
    }

    //move from coll to coll_out and getting children of each in the same time. (deep first)
    for (const std::pair<size_t, bool> &idx : better_chain) {
        
        if (idx.first >= loops.size()) {
            // this is a thin wall
            // let's get it from the sorted collection as it might have been reversed
            coll_out.set_entities().reserve(coll_out.entities().size() + 1);
            coll_out.set_entities().emplace_back(coll[idx.first]);
            coll[idx.first] = nullptr;
            if (idx.second)
                coll_out.entities().back()->reverse();
            //if thin extrusion is a loop, make it ccw like a normal contour.
            if (ExtrusionLoop* loop = dynamic_cast<ExtrusionLoop*>(coll_out.entities().back())) {
                loop->make_counter_clockwise();
            }
        } else {
            const PerimeterGeneratorLoop &loop = loops[idx.first];
            assert(thin_walls.empty());
            ExtrusionEntityCollection children = this->_traverse_loops(loop.children, thin_walls);
            coll_out.set_entities().reserve(coll_out.entities().size() + children.entities().size() + 1);
            ExtrusionLoop *eloop = static_cast<ExtrusionLoop*>(coll[idx.first]);
            coll[idx.first] = nullptr;
            if (loop.is_contour) {
                //note: this->layer->id() % 2 == 1 already taken into account in the is_steep_overhang compute (to save time).
                if (loop.is_steep_overhang && this->layer->id() % 2 == 1)
                    eloop->make_clockwise();
                else
                    eloop->make_counter_clockwise();
                coll_out.append(std::move(children.entities()));
                coll_out.append(*eloop);
            } else {
                eloop->make_clockwise();
                coll_out.append(*eloop);
                coll_out.append(std::move(children.entities()));
            }
        }
    }
    return coll_out;
}

void PerimeterGenerator::_merge_thin_walls(ExtrusionEntityCollection &extrusions, ThickPolylines &thin_walls) const {
    //TODO: find a way to avoid double copy (from EntityCollection to ChangeFlow to searcher.search_result.loop
    class ChangeFlow : public ExtrusionVisitor {
    public:
        float percent_extrusion;
        std::vector<ExtrusionPath> paths;
        Point* first_point = nullptr;
        coordf_t resolution_sqr;
        virtual void use(ExtrusionPath &path) override {
            //ensure the loop is continue.
            if (first_point != nullptr) {
                if (*first_point != path.first_point()) {
                    if (first_point->distance_to_square(path.first_point()) < resolution_sqr) {
                        path.polyline.points[0] = *first_point;
                    } else {
                        //add travel
                        ExtrusionPath travel(path.role());
                        travel.width = path.width;
                        travel.height = path.height;
                        travel.mm3_per_mm = 0;
                        travel.polyline.points.push_back(*first_point);
                        travel.polyline.points.push_back(path.first_point());
                        paths.push_back(travel);
                    }
                }
                first_point = nullptr;
            }
            path.mm3_per_mm *= percent_extrusion;
            path.width *= percent_extrusion;
            paths.push_back(path);
        }
        virtual void use(ExtrusionPath3D &path3D) override { /*shouldn't happen*/ }
        virtual void use(ExtrusionMultiPath &multipath) override { /*shouldn't happen*/ }
        virtual void use(ExtrusionMultiPath3D &multipath) { /*shouldn't happen*/ }
        virtual void use(ExtrusionLoop &loop) override {
            for (ExtrusionPath &path : loop.paths)
                this->use(path);
        }
        virtual void use(ExtrusionEntityCollection &collection) override {
            for (ExtrusionEntity *entity : collection.entities())
                entity->visit(*this);
        }
    };
    struct BestPoint {
        //Point p;
        ExtrusionPath *path;
        size_t idx_path;
        ExtrusionLoop *loop;
        size_t idx_line;
        Line line;
        double dist;
        bool from_start;
    };
    //use a visitor to found the best point.
    class SearchBestPoint : public ExtrusionVisitor {
    public:
        ThickPolyline* thin_wall;
        BestPoint search_result;
        size_t idx_path;
        ExtrusionLoop *current_loop = nullptr;
        virtual void use(ExtrusionPath &path) override {
            //don't consider other thin walls.
            if (path.role() == erThinWall) return;
            //for each segment
            Lines lines = path.polyline.lines();
            for (size_t idx_line = 0; idx_line < lines.size(); idx_line++) {
                //look for nearest point
                double dist = lines[idx_line].distance_to_squared(thin_wall->points.front());
                if (dist < search_result.dist) {
                    search_result.path = &path;
                    search_result.idx_path = idx_path;
                    search_result.idx_line = idx_line;
                    search_result.line = lines[idx_line];
                    search_result.dist = dist;
                    search_result.from_start = true;
                    search_result.loop = current_loop;
                }
                dist = lines[idx_line].distance_to_squared(thin_wall->points.back());
                if (dist < search_result.dist) {
                    search_result.path = &path;
                    search_result.idx_path = idx_path;
                    search_result.idx_line = idx_line;
                    search_result.line = lines[idx_line];
                    search_result.dist = dist;
                    search_result.from_start = false;
                    search_result.loop = current_loop;
                }
            }
        }
        virtual void use(ExtrusionPath3D &path3D) override { /*shouldn't happen*/ }
        virtual void use(ExtrusionMultiPath &multipath) override { /*shouldn't happen*/ }
        virtual void use(ExtrusionMultiPath3D &multipath) { /*shouldn't happen*/ }
        virtual void use(ExtrusionLoop &loop) override {
            ExtrusionLoop * last_loop = current_loop;
            current_loop = &loop;
            //for each extrusion path
            idx_path = 0;
            for (ExtrusionPath &path : loop.paths) {
                this->use(path);
                idx_path++;
            }
            current_loop = last_loop;
        }
        virtual void use(ExtrusionEntityCollection &collection) override {
            collection.set_can_sort_reverse(true, true);
            //for each loop? (or other collections)
            for (ExtrusionEntity *entity : collection.entities())
                entity->visit(*this);
        }
    };
    //max dist to branch: ~half external periemeter width
    coord_t max_width = this->ext_perimeter_flow.scaled_width();
    SearchBestPoint searcher;
    ThickPolylines not_added;
    //search the best extusion/point to branch into
     //for each thin wall
    int idx = 0;
    for (ThickPolyline &tw : thin_walls) {
        searcher.thin_wall = &tw;
        searcher.search_result.dist = double(max_width);
        searcher.search_result.dist *= searcher.search_result.dist;
        searcher.search_result.path = nullptr;
        searcher.use(extrusions);
        idx++;
        //now insert thin wall if it has a point
        //it found a segment
        if (searcher.search_result.path != nullptr) {
#if _DEBUG
            searcher.search_result.loop->visit(LoopAssertVisitor{});
#endif
            if (!searcher.search_result.from_start)
                tw.reverse();
            //get the point
            Point point = tw.points.front().projection_onto(searcher.search_result.line);
            //we have to create 3 paths: 1: thinwall extusion, 2: thinwall return, 3: end of the path
            //create new path : end of the path
            Polyline poly_after;
            poly_after.points.push_back(point);
            poly_after.points.insert(poly_after.points.end(),
                searcher.search_result.path->polyline.points.begin() + searcher.search_result.idx_line + 1,
                searcher.search_result.path->polyline.points.end());
            searcher.search_result.path->polyline.points.erase(
                searcher.search_result.path->polyline.points.begin() + searcher.search_result.idx_line + 1,
                searcher.search_result.path->polyline.points.end());
            searcher.search_result.loop->paths[searcher.search_result.idx_path].polyline.points.push_back(point);
            searcher.search_result.loop->paths.insert(searcher.search_result.loop->paths.begin() + 1 + searcher.search_result.idx_path, 
                ExtrusionPath(poly_after, *searcher.search_result.path));
            //create thin wall path exttrusion
            ExtrusionEntityCollection tws;
            tws.append(Geometry::thin_variable_width({ tw }, erThinWall, this->ext_perimeter_flow, std::max(ext_perimeter_flow.scaled_width() / 4, scale_t(this->print_config->resolution))));
            assert(!tws.entities().empty());
            ChangeFlow change_flow;
            if (tws.entities().size() == 1 && tws.entities()[0]->is_loop()) {
                //loop, just add it 
                change_flow.first_point = &point;
                change_flow.percent_extrusion = 1;
                change_flow.use(tws);
                //add move back
                searcher.search_result.loop->paths.insert(searcher.search_result.loop->paths.begin() + 1 + searcher.search_result.idx_path,
                    change_flow.paths.begin(), change_flow.paths.end());
                //add move to

#if _DEBUG
                searcher.search_result.loop->visit(LoopAssertVisitor{});
#endif
            } else {
                //first add the return path
                //ExtrusionEntityCollection tws_second = tws; // this does a deep copy
                change_flow.first_point = &point;
                change_flow.percent_extrusion = 0.1f;
                change_flow.use(tws); // tws_second); //does not need the deep copy if the change_flow copy the content instead of re-using it.
                //force reverse
                for (ExtrusionPath &path : change_flow.paths)
                    path.reverse();
                std::reverse(change_flow.paths.begin(), change_flow.paths.end());
                //std::reverse(change_flow.paths.begin(), change_flow.paths.end());
                searcher.search_result.loop->paths.insert(searcher.search_result.loop->paths.begin() + 1 + searcher.search_result.idx_path,
                    change_flow.paths.begin(), change_flow.paths.end());
                //add the real extrusion path
                change_flow.first_point = &point;
                change_flow.percent_extrusion = 9.f; // 0.9 but as we modified it by 0.1 just before, has to multiply by 10
                change_flow.paths = std::vector<ExtrusionPath>();
                change_flow.use(tws);
                searcher.search_result.loop->paths.insert(searcher.search_result.loop->paths.begin() + 1 + searcher.search_result.idx_path,
                    change_flow.paths.begin(), change_flow.paths.end());
#if _DEBUG
                searcher.search_result.loop->visit(LoopAssertVisitor{});
#endif
            }
        } else {
            not_added.push_back(tw);
        }
    }

    //now add thinwalls that have no anchor (make them reversable)
    extrusions.append(Geometry::thin_variable_width(not_added, erThinWall, this->ext_perimeter_flow, std::max(ext_perimeter_flow.scaled_width() / 4, scale_t(this->print_config->resolution))));
}

PerimeterIntersectionPoint
PerimeterGenerator::_get_nearest_point(const PerimeterGeneratorLoops &children, ExtrusionLoop &myPolylines, const coord_t dist_cut, const coord_t max_dist) const {
    //find best points of intersections
    PerimeterIntersectionPoint intersect;
    intersect.distance = 0x7FFFFFFF; // ! assumption on intersect type & max value
    intersect.idx_polyline_outter = -1;
    intersect.idx_children = -1;
    for (size_t idx_child = 0; idx_child < children.size(); idx_child++) {
        const PerimeterGeneratorLoop &child = children[idx_child];
        for (size_t idx_poly = 0; idx_poly < myPolylines.paths.size(); idx_poly++) {
            //if (myPolylines.paths[idx_poly].extruder_id == (unsigned int)-1) continue;
            if (myPolylines.paths[idx_poly].length() < dist_cut + perimeter_flow.scaled_width()/20) continue;

            if ((myPolylines.paths[idx_poly].role() == erExternalPerimeter || child.is_external() )
                && (this->object_config->seam_position.value != SeamPosition::spRandom && this->object_config->seam_position.value != SeamPosition::spAllRandom)) {
                //first, try to find 2 point near enough
                for (size_t idx_point = 0; idx_point < myPolylines.paths[idx_poly].polyline.points.size(); idx_point++) {
                    const Point &p = myPolylines.paths[idx_poly].polyline.points[idx_point];
                    const Point &nearest_p = *child.polygon.closest_point(p);
                    const double dist = nearest_p.distance_to(p);
                    //Try to find a point in the far side, aligning them
                    if (dist + dist_cut / 20 < intersect.distance || 
                        (config->perimeter_loop_seam.value == spRear && (intersect.idx_polyline_outter <0 || p.y() > intersect.outter_best.y())
                            && dist <= max_dist && intersect.distance + dist_cut / 20)) {
                        //ok, copy the idx
                        intersect.distance = (coord_t)nearest_p.distance_to(p);
                        intersect.idx_children = idx_child;
                        intersect.idx_polyline_outter = idx_poly;
                        intersect.outter_best = p;
                        intersect.child_best = nearest_p;
                    }
                }
            } else {
                //first, try to find 2 point near enough
                for (size_t idx_point = 0; idx_point < myPolylines.paths[idx_poly].polyline.points.size(); idx_point++) {
                    const Point &p = myPolylines.paths[idx_poly].polyline.points[idx_point];
                    const Point &nearest_p = *child.polygon.closest_point(p);
                    const double dist = nearest_p.distance_to(p);
                    if (dist + SCALED_EPSILON < intersect.distance || 
                        (config->perimeter_loop_seam.value == spRear && (intersect.idx_polyline_outter<0 || p.y() < intersect.outter_best.y())
                            && dist <= max_dist && intersect.distance + dist_cut / 20)) {
                        //ok, copy the idx
                        intersect.distance = (coord_t)nearest_p.distance_to(p);
                        intersect.idx_children = idx_child;
                        intersect.idx_polyline_outter = idx_poly;
                        intersect.outter_best = p;
                        intersect.child_best = nearest_p;
                    }
                }
            }
        }
    }
    if (intersect.distance <= max_dist) {
        return intersect;
    }

    for (size_t idx_child = 0; idx_child < children.size(); idx_child++) {
        const PerimeterGeneratorLoop &child = children[idx_child];
        for (size_t idx_poly = 0; idx_poly < myPolylines.paths.size(); idx_poly++) {
            //if (myPolylines.paths[idx_poly].extruder_id == (unsigned int)-1) continue;
            if (myPolylines.paths[idx_poly].length() < dist_cut + perimeter_flow.scaled_width() / 20) continue;

            //second, try to check from one of my points
            //don't check the last point, as it's used to go outter, can't use it to go inner.
            for (size_t idx_point = 1; idx_point < myPolylines.paths[idx_poly].polyline.points.size()-1; idx_point++) {
                const Point &p = myPolylines.paths[idx_poly].polyline.points[idx_point];
                Point nearest_p = child.polygon.point_projection(p);
                coord_t dist = (coord_t)nearest_p.distance_to(p);
                //if no projection, go to next
                if (dist == 0) continue;
                if (dist + SCALED_EPSILON / 2 < intersect.distance) {
                    //ok, copy the idx
                    intersect.distance = dist;
                    intersect.idx_children = idx_child;
                    intersect.idx_polyline_outter = idx_poly;
                    intersect.outter_best = p;
                    intersect.child_best = nearest_p;
                }
            }
        }
    }
    if (intersect.distance <= max_dist) {
        return intersect;
    }

    for (size_t idx_child = 0; idx_child < children.size(); idx_child++) {
        const PerimeterGeneratorLoop &child = children[idx_child];
        for (size_t idx_poly = 0; idx_poly < myPolylines.paths.size(); idx_poly++) {
            //if (myPolylines.paths[idx_poly].extruder_id == (unsigned int)-1) continue;
            if (myPolylines.paths[idx_poly].length() < dist_cut + perimeter_flow.scaled_width() / 20) continue;

            //lastly, try to check from one of his points
            for (size_t idx_point = 0; idx_point < child.polygon.points.size(); idx_point++) {
                const Point &p = child.polygon.points[idx_point];
                Point nearest_p = myPolylines.paths[idx_poly].polyline.point_projection(p);
                coord_t dist = (coord_t)nearest_p.distance_to(p);
                //if no projection, go to next
                if (dist == 0) continue;
                if (dist + SCALED_EPSILON / 2 < intersect.distance) {
                    //ok, copy the idx
                    intersect.distance = dist;
                    intersect.idx_children = idx_child;
                    intersect.idx_polyline_outter = idx_poly;
                    intersect.outter_best = nearest_p;
                    intersect.child_best = p;
                }
            }
        }
    }
    return intersect;
}


ExtrusionLoop
PerimeterGenerator::_extrude_and_cut_loop(const PerimeterGeneratorLoop &loop, const Point entry_point, const Line &direction, bool enforce_loop) const
{

    bool need_to_reverse = false;
    Polyline initial_polyline;
    coord_t dist_cut = (coord_t)scale_(this->print_config->nozzle_diameter.get_at(this->config->perimeter_extruder - 1));

    //fuzzify first in this case, as it's a bit complicated to do it after.
    Polygon fuzzy_poly;
    if (loop.fuzzify) {
        fuzzy_poly = loop.polygon;
        double nozle_diameter = loop.is_external() ? this->ext_perimeter_flow.nozzle_diameter() : this->perimeter_flow.nozzle_diameter();
        double fuzzy_skin_thickness = config->fuzzy_skin_thickness.get_abs_value(nozle_diameter);
        double fuzzy_skin_point_dist = config->fuzzy_skin_point_dist.get_abs_value(nozle_diameter);
        fuzzy_polygon(fuzzy_poly, scale_d(fuzzy_skin_thickness), scale_d(fuzzy_skin_point_dist));
    }
    const Polygon& poly_to_use = loop.fuzzify ? fuzzy_poly : loop.polygon;

    if (poly_to_use.points.size() < 3) return ExtrusionLoop(elrDefault);
    if (poly_to_use.length() < dist_cut * 2) {
        if (enforce_loop) {
            //do something to still use it
            dist_cut = poly_to_use.length() / 4;
        } else {
            //reduce it ot a single-point loop that will eb emrged inside the complex path
            ExtrusionLoop single_point(elrDefault);
            Polyline poly_point;
            poly_point.append(poly_to_use.centroid());
            single_point.paths.emplace_back(
                loop.is_external() ? erExternalPerimeter : erPerimeter,
                (double)(loop.is_external() ? this->ext_mm3_per_mm() : this->mm3_per_mm()),
                (float)(loop.is_external() ? this->ext_perimeter_flow.width() : this->perimeter_flow.width()),
                (float)(this->layer->height));
            single_point.paths.back().polyline = poly_point;
            return single_point;
        }
    }
    const size_t idx_closest_from_entry_point = poly_to_use.closest_point_index(entry_point);
    if (poly_to_use.points[idx_closest_from_entry_point].distance_to(entry_point) > SCALED_EPSILON * 2) {
        //create new Point
        //get first point
        size_t idx_before = -1;
        for (size_t idx_p_a = 0; idx_p_a < poly_to_use.points.size(); ++idx_p_a) {
            Line l(poly_to_use.points[idx_p_a], poly_to_use.points[(idx_p_a + 1 == poly_to_use.points.size()) ? 0 : (idx_p_a + 1)]);
            if (entry_point.distance_to(l) < SCALED_EPSILON) {
                idx_before = idx_p_a;
                break;
            }
        }
        if (idx_before == (size_t)-1) std::cerr << "ERROR: _traverse_and_join_loops : idx_before can't be finded to create new point\n";
        initial_polyline = poly_to_use.split_at_index(idx_before);
        initial_polyline.points.push_back(entry_point);
        initial_polyline.points[0] = entry_point;
    } else {
        initial_polyline = poly_to_use.split_at_index(idx_closest_from_entry_point);
    }
   

    //std::vector<PerimeterPolylineNode> myPolylines;
    ExtrusionLoop my_loop;

    //overhang / notoverhang
    {
        bool is_external = loop.is_external();

        ExtrusionRole role;
        ExtrusionLoopRole loop_role;
        role = is_external ? erExternalPerimeter : erPerimeter;
        if (loop.is_internal_contour()) {
            // Note that we set loop role to ContourInternalPerimeter
            // also when loop is both internal and external (i.e.
            // there's only one contour loop).
            loop_role = elrInternal;
        } else {
            loop_role = elrDefault;
        }
        if (!loop.is_contour) {
            loop_role = (ExtrusionLoopRole)(loop_role | elrHole);
        }

        // detect overhanging/bridging perimeters
        if ( this->config->overhangs_width_speed.value > 0 && this->layer->id() > 0
            && !(this->object_config->support_material && this->object_config->support_material_contact_distance_type.value == zdNone)) {
            ExtrusionPaths paths = this->create_overhangs(initial_polyline, role, is_external);
            
            if (direction.length() > 0) {
                Polyline direction_polyline;
                for (ExtrusionPath &path : paths) {
                    if(direction_polyline.size() == 0 || direction_polyline.points.back() != path.first_point())
                        direction_polyline.points.insert(direction_polyline.points.end(), path.polyline.points.begin(), path.polyline.points.end());
                }
                for (int i = 0; i < direction_polyline.points.size() - 1; i++)
                    assert(direction_polyline.points[i] != direction_polyline.points[i + 1]);
                if (direction_polyline.length() > perimeter_flow.scaled_width() / 8) {
                    direction_polyline.clip_start(perimeter_flow.scaled_width() / 20);
                    direction_polyline.clip_end(perimeter_flow.scaled_width() / 20);
                }
                coord_t dot = direction.dot(Line(direction_polyline.points.back(), direction_polyline.points.front()));
                need_to_reverse = dot>0;
            }
            if (need_to_reverse) {
                std::reverse(paths.begin(), paths.end());
            }
            //search for the first path
            size_t good_idx = 0; 
            for (size_t idx_path = 0; idx_path < paths.size(); idx_path++) {
                const ExtrusionPath &path = paths[idx_path];
                if (need_to_reverse) {
                    if (path.polyline.points.back().coincides_with_epsilon(initial_polyline.points.front())) {
                        good_idx = idx_path;
                        break;
                    }
                } else {
                    if (path.polyline.points.front().coincides_with_epsilon(initial_polyline.points.front())) {
                        good_idx = idx_path;
                        break;
                    }
                }
            }
            for (size_t idx_path = good_idx; idx_path < paths.size(); idx_path++) {
                ExtrusionPath &path = paths[idx_path];
                if (need_to_reverse) path.reverse();
                my_loop.paths.emplace_back(path);
            }
            for (size_t idx_path = 0; idx_path < good_idx; idx_path++) {
                ExtrusionPath &path = paths[idx_path];
                if (need_to_reverse) path.reverse();
                my_loop.paths.emplace_back(path);
            }
        } else {

            if (direction.length() > 0) {
                Polyline direction_polyline = initial_polyline;
                direction_polyline.clip_start(perimeter_flow.scaled_width() / 20);
                direction_polyline.clip_end(perimeter_flow.scaled_width() / 20);
                coord_t dot = direction.dot(Line(direction_polyline.points.back(), direction_polyline.points.front()));
                need_to_reverse = dot>0;
            }

            ExtrusionPath path(role);
            path.polyline = initial_polyline;
            if (need_to_reverse) path.polyline.reverse();
            path.mm3_per_mm = is_external ? this->ext_mm3_per_mm() : this->mm3_per_mm();
            path.width = is_external ? this->ext_perimeter_flow.width() : this->perimeter_flow.width();
            path.height = (float)(this->layer->height);
            my_loop.paths.emplace_back(path);
        }

    }
    
    return my_loop;
}

ExtrusionLoop
PerimeterGenerator::_traverse_and_join_loops(const PerimeterGeneratorLoop &loop, const PerimeterGeneratorLoops &children, const Point entry_point) const
{
    //std::cout << " === ==== _traverse_and_join_loops ==== ===\n";
    // other perimeters
    //this->_mm3_per_mm = this->perimeter_flow.mm3_per_mm();
    //coord_t perimeter_width = this->perimeter_flow.scaled_width();
    const coord_t perimeter_spacing = this->perimeter_flow.scaled_spacing();

    //// external perimeters
    //this->_ext_mm3_per_mm = this->ext_perimeter_flow.mm3_per_mm();
    //coord_t ext_perimeter_width = this->ext_perimeter_flow.scaled_width();
    const coord_t ext_perimeter_spacing = this->ext_perimeter_flow.scaled_spacing();
    //coord_t ext_perimeter_spacing2 = this->ext_perimeter_flow.scaled_spacing(this->perimeter_flow);

    //const coord_t dist_cut = (coord_t)scale_(this->print_config->nozzle_diameter.get_at(this->config->perimeter_extruder - 1));
    //TODO change this->external_perimeter_flow.scaled_width() if it's the first one!
    const coord_t max_width_extrusion = this->perimeter_flow.scaled_width();
    ExtrusionLoop my_loop = _extrude_and_cut_loop(loop, entry_point, Line{ {0,0},{0,0} }, true);

    int child_idx = 0;
    //Polylines myPolylines = { myPolyline };
    //iterate on each point ot find the best place to go into the child
    PerimeterGeneratorLoops childs = children;
    while (!childs.empty()) {
        child_idx++;
        PerimeterIntersectionPoint nearest = this->_get_nearest_point(childs, my_loop, coord_t(this->perimeter_flow.scaled_width()), coord_t(this->perimeter_flow.scaled_width()* 1.42));
        if (nearest.idx_children == (size_t)-1) {
            //return ExtrusionEntityCollection();
            break;
        } else {
            const PerimeterGeneratorLoop &child = childs[nearest.idx_children];
            //std::cout << "c." << child_idx << " === i have " << my_loop.paths.size() << " paths" << " == cut_path_is_ccw size " << path_is_ccw.size() << "\n";
            //std::cout << "change to child " << nearest.idx_children << " @ " << unscale(nearest.outter_best.x) << ":" << unscale(nearest.outter_best.y)
            //    << ", idxpolyline = " << nearest.idx_polyline_outter << "\n";
            //PerimeterGeneratorLoops less_childs = childs;
            //less_childs.erase(less_childs.begin() + nearest.idx_children);
            //create new node with recursive ask for the inner perimeter & COPY of the points, ready to be cut
            my_loop.paths.insert(my_loop.paths.begin() + nearest.idx_polyline_outter + 1, my_loop.paths[nearest.idx_polyline_outter]);

            // outer_start == outer_end
            ExtrusionPath *outer_start = &my_loop.paths[nearest.idx_polyline_outter];
            ExtrusionPath *outer_end = &my_loop.paths[nearest.idx_polyline_outter + 1];
            Line deletedSection;

            //cut our polyline, so outer_start has no common point with outer_end
            //separate them
            size_t nearest_idx_outter = outer_start->polyline.closest_point_index(nearest.outter_best);
            if (outer_start->polyline.points[nearest_idx_outter].coincides_with_epsilon(nearest.outter_best)) {
                if (nearest_idx_outter < outer_start->polyline.points.size() - 1) {
                    outer_start->polyline.points.erase(outer_start->polyline.points.begin() + nearest_idx_outter + 1, outer_start->polyline.points.end());
                }
                if (nearest_idx_outter > 0) {
                    outer_end->polyline.points.erase(outer_end->polyline.points.begin(), outer_end->polyline.points.begin() + nearest_idx_outter);
                }
            } else {
                //get first point
                size_t idx_before = -1;
                for (size_t idx_p_a = 0; idx_p_a < outer_start->polyline.points.size() - 1; ++idx_p_a) {
                    Line l(outer_start->polyline.points[idx_p_a], outer_start->polyline.points[idx_p_a + 1]);
                    if (nearest.outter_best.distance_to(l) < SCALED_EPSILON) {
                        idx_before = idx_p_a;
                        break;
                    }
                }
                if (idx_before == (size_t)-1) {
                    std::cerr << "ERROR: idx_before can't be finded\n";
                    continue;
                }

                Points &my_polyline_points = outer_start->polyline.points;
                my_polyline_points.erase(my_polyline_points.begin() + idx_before + 1, my_polyline_points.end());
                my_polyline_points.push_back(nearest.outter_best);

                if (idx_before < outer_end->polyline.points.size()-1)
                    outer_end->polyline.points.erase(outer_end->polyline.points.begin(), outer_end->polyline.points.begin() + idx_before + 1);
                else
                    outer_end->polyline.points.erase(outer_end->polyline.points.begin()+1, outer_end->polyline.points.end());
                outer_end->polyline.points.insert(outer_end->polyline.points.begin(), nearest.outter_best);
            }
            Polyline to_reduce = outer_start->polyline;
            if (to_reduce.points.size()>1 && to_reduce.length() > (perimeter_flow.scaled_width() / 10)) to_reduce.clip_end(perimeter_flow.scaled_width() / 20);
            deletedSection.a = to_reduce.points.back();
            to_reduce = outer_end->polyline;
            if (to_reduce.points.size()>1 && to_reduce.length() > (perimeter_flow.scaled_width() / 10)) to_reduce.clip_start(perimeter_flow.scaled_width() / 20);
            deletedSection.b = to_reduce.points.front();
            
            //get the inner loop to connect to us.
            ExtrusionLoop child_loop = _extrude_and_cut_loop(child, nearest.child_best, deletedSection);

            const coord_t inner_child_spacing = child.is_external() ? ext_perimeter_spacing : perimeter_spacing;
            const coord_t outer_start_spacing = scale_(outer_start->width - outer_start->height * (1. - 0.25 * PI));
            const coord_t outer_end_spacing = scale_(outer_end->width - outer_end->height * (1. - 0.25 * PI));

            //FIXME: if child_loop has no point or 1 point or not enough space !!!!!!!
            const size_t child_paths_size = child_loop.paths.size();
            if (child_paths_size == 0) continue;
            my_loop.paths.insert(my_loop.paths.begin() + nearest.idx_polyline_outter + 1, child_loop.paths.begin(), child_loop.paths.end());
            
            //add paths into my_loop => need to re-get the refs
            outer_start = &my_loop.paths[nearest.idx_polyline_outter];
            outer_end = &my_loop.paths[nearest.idx_polyline_outter + child_paths_size + 1];
            ExtrusionPath *inner_start = &my_loop.paths[nearest.idx_polyline_outter+1];
            ExtrusionPath *inner_end = &my_loop.paths[nearest.idx_polyline_outter + child_paths_size];
            //TRIM
            //choose trim direction
            if (outer_start->polyline.points.size() == 1 && outer_end->polyline.points.size() == 1) {
                //do nothing
            } else if (outer_start->polyline.points.size() == 1) {
                outer_end->polyline.clip_start(double(outer_end_spacing));
                if (inner_end->polyline.length() > inner_child_spacing)
                    inner_end->polyline.clip_end(double(inner_child_spacing));
                else
                    inner_end->polyline.clip_end(inner_end->polyline.length() / 2);
            } else if (outer_end->polyline.points.size() == 1) {
                outer_start->polyline.clip_end(double(outer_start_spacing));
                if (inner_start->polyline.length() > inner_child_spacing)
                    inner_start->polyline.clip_start(double(inner_child_spacing));
                else
                    inner_start->polyline.clip_start(inner_start->polyline.length()/2);
            } else {
                coord_t length_poly_1 = (coord_t)outer_start->polyline.length();
                coord_t length_poly_2 = (coord_t)outer_end->polyline.length();
                coord_t length_trim_1 = outer_start_spacing / 2;
                coord_t length_trim_2 = outer_end_spacing / 2;
                if (length_poly_1 < length_trim_1) {
                    length_trim_2 = length_trim_1 + length_trim_2 - length_poly_1;
                }
                if (length_poly_2 < length_trim_1) {
                    length_trim_1 = length_trim_1 + length_trim_2 - length_poly_2;
                }
                if (length_poly_1 > length_trim_1) {
                    outer_start->polyline.clip_end(double(length_trim_1));
                } else {
                    outer_start->polyline.points.erase(outer_start->polyline.points.begin() + 1, outer_start->polyline.points.end());
                }
                if (length_poly_2 > length_trim_2) {
                    outer_end->polyline.clip_start(double(length_trim_2));
                } else {
                    outer_end->polyline.points.erase(outer_end->polyline.points.begin(), outer_end->polyline.points.end() - 1);
                }
                
                length_poly_1 = coord_t(inner_start->polyline.length());
                length_poly_2 = coord_t(inner_end->polyline.length());
                length_trim_1 = inner_child_spacing / 2;
                length_trim_2 = inner_child_spacing / 2;
                if (length_poly_1 < length_trim_1) {
                    length_trim_2 = length_trim_1 + length_trim_2 - length_poly_1;
                }
                if (length_poly_2 < length_trim_1) {
                    length_trim_1 = length_trim_1 + length_trim_2 - length_poly_2;
                }
                if (length_poly_1 > length_trim_1) {
                    inner_start->polyline.clip_start(double(length_trim_1));
                } else {
                    inner_start->polyline.points.erase(
                        inner_start->polyline.points.begin(), 
                        inner_start->polyline.points.end() - 1);
                }
                if (length_poly_2 > length_trim_2) {
                    inner_end->polyline.clip_end(double(length_trim_2));
                } else {
                    inner_end->polyline.points.erase(
                        inner_end->polyline.points.begin() + 1,
                        inner_end->polyline.points.end());
                }
            }

            //last check to see if we need a reverse
            {
                Line l1(outer_start->polyline.points.back(), inner_start->polyline.points.front());
                Line l2(inner_end->polyline.points.back(), outer_end->polyline.points.front());
                Point p_inter(0, 0);
                bool is_interect = l1.intersection(l2, &p_inter);
                if (is_interect && p_inter.distance_to(l1) < SCALED_EPSILON && p_inter.distance_to(l2) < SCALED_EPSILON) {
                    //intersection! need to reverse!
                    std::reverse(my_loop.paths.begin() + nearest.idx_polyline_outter + 1, my_loop.paths.begin() + nearest.idx_polyline_outter + child_paths_size + 1);
                    for (size_t idx = nearest.idx_polyline_outter + 1; idx < nearest.idx_polyline_outter + child_paths_size + 1; idx++) {
                        my_loop.paths[idx].reverse();
                    }
                    outer_start = &my_loop.paths[nearest.idx_polyline_outter];
                    inner_start = &my_loop.paths[nearest.idx_polyline_outter + 1];
                    inner_end = &my_loop.paths[nearest.idx_polyline_outter + child_paths_size];
                    outer_end = &my_loop.paths[nearest.idx_polyline_outter + child_paths_size + 1];
                }

            }

            //now add extrusionPAths to connect the two loops
            ExtrusionPaths travel_path_begin;// (ExtrusionRole::erNone, 0, outer_start->width, outer_start->height);
            //travel_path_begin.extruder_id = -1;
            ExtrusionPaths travel_path_end;// (ExtrusionRole::erNone, 0, outer_end->width, outer_end->height);
            //travel_path_end.extruder_id = -1;
            double dist_travel = outer_start->polyline.points.back().distance_to(inner_start->polyline.points.front());
            if (dist_travel > max_width_extrusion*1.5 && this->config->fill_density.value > 0) {
                travel_path_begin.emplace_back(ExtrusionRole::erPerimeter, outer_start->mm3_per_mm, outer_start->width, outer_start->height);
                travel_path_begin.emplace_back(ExtrusionRole::erNone, 0, outer_start->width, outer_start->height);
                travel_path_begin.emplace_back(ExtrusionRole::erPerimeter, outer_start->mm3_per_mm, outer_start->width, outer_start->height);
                //travel_path_begin[0].extruder_id = -1;
                //travel_path_begin[1].extruder_id = -1;
                //travel_path_begin[2].extruder_id = -1;
                Line line(outer_start->polyline.points.back(), inner_start->polyline.points.front());
                Point p_dist_cut_extrude = (line.b - line.a);
                p_dist_cut_extrude.x() = (coord_t)(p_dist_cut_extrude.x() * ((double)max_width_extrusion) / (line.length() * 2));
                p_dist_cut_extrude.y() = (coord_t)(p_dist_cut_extrude.y() * ((double)max_width_extrusion) / (line.length() * 2));
                //extrude a bit after the turn, to close the loop
                Point p_start_travel = line.a;
                p_start_travel += p_dist_cut_extrude;
                travel_path_begin[0].polyline.append(outer_start->polyline.points.back());
                travel_path_begin[0].polyline.append(p_start_travel);
                //extrude a bit before the final turn, to close the loop
                Point p_end_travel = line.b;
                p_end_travel -= p_dist_cut_extrude;
                travel_path_begin[2].polyline.append(p_end_travel);
                travel_path_begin[2].polyline.append(inner_start->polyline.points.front());
                //fake travel in the middle
                travel_path_begin[1].polyline.append(p_start_travel);
                travel_path_begin[1].polyline.append(p_end_travel);
            } else {
                // the path is small enough to extrude all along.
                double flow_mult = 1;
                if (dist_travel > max_width_extrusion && this->config->fill_density.value > 0) {
                    // the path is a bit too long, reduce the extrusion flow.
                    flow_mult = max_width_extrusion / dist_travel;
                }
                travel_path_begin.emplace_back(ExtrusionRole::erPerimeter, outer_start->mm3_per_mm * flow_mult, (float)(outer_start->width * flow_mult), outer_start->height);
                //travel_path_begin[0].extruder_id = -1;
                travel_path_begin[0].polyline.append(outer_start->polyline.points.back());
                travel_path_begin[0].polyline.append(inner_start->polyline.points.front());
            }
            dist_travel = inner_end->polyline.points.back().distance_to(outer_end->polyline.points.front());
            if (dist_travel > max_width_extrusion*1.5 && this->config->fill_density.value > 0) {
                travel_path_end.emplace_back(ExtrusionRole::erPerimeter, outer_end->mm3_per_mm, outer_end->width, outer_end->height);
                travel_path_end.emplace_back(ExtrusionRole::erNone, 0, outer_end->width, outer_end->height);
                travel_path_end.emplace_back(ExtrusionRole::erPerimeter, outer_end->mm3_per_mm, outer_end->width, outer_end->height);
                //travel_path_end[0].extruder_id = -1;
                //travel_path_end[1].extruder_id = -1;
                //travel_path_end[2].extruder_id = -1;
                Line line(inner_end->polyline.points.back(), outer_end->polyline.points.front());
                Point p_dist_cut_extrude = (line.b - line.a);
                p_dist_cut_extrude.x() = (coord_t)(p_dist_cut_extrude.x() * ((double)max_width_extrusion) / (line.length() * 2));
                p_dist_cut_extrude.y() = (coord_t)(p_dist_cut_extrude.y() * ((double)max_width_extrusion) / (line.length() * 2));
                //extrude a bit after the turn, to close the loop
                Point p_start_travel_2 = line.a;
                p_start_travel_2 += p_dist_cut_extrude;
                travel_path_end[0].polyline.append(inner_end->polyline.points.back());
                travel_path_end[0].polyline.append(p_start_travel_2);
                //extrude a bit before the final turn, to close the loop
                Point p_end_travel_2 = line.b;
                p_end_travel_2 -= p_dist_cut_extrude;
                travel_path_end[2].polyline.append(p_end_travel_2);
                travel_path_end[2].polyline.append(outer_end->polyline.points.front());
                //fake travel in the middle
                travel_path_end[1].polyline.append(p_start_travel_2);
                travel_path_end[1].polyline.append(p_end_travel_2);
            } else {
                // the path is small enough to extrude all along.
                double flow_mult = 1;
                if (dist_travel > max_width_extrusion && this->config->fill_density.value > 0) {
                    // the path is a bit too long, reduce the extrusion flow.
                    flow_mult = max_width_extrusion / dist_travel;
                }
                travel_path_end.emplace_back(ExtrusionRole::erPerimeter, outer_end->mm3_per_mm * flow_mult, (float)(outer_end->width * flow_mult), outer_end->height);
                //travel_path_end[0].extruder_id = -1;
                travel_path_end[0].polyline.append(inner_end->polyline.points.back());
                travel_path_end[0].polyline.append(outer_end->polyline.points.front());
            }
            //check if we add path or reuse bits
            //FIXME
            /*if (outer_start->polyline.points.size() == 1) {
                outer_start->polyline = travel_path_begin.front().polyline;
                travel_path_begin.erase(travel_path_begin.begin());
                outer_start->extruder_id = -1;
            } else if (outer_end->polyline.points.size() == 1) {
                outer_end->polyline = travel_path_end.back().polyline;
                travel_path_end.erase(travel_path_end.end() - 1);
                outer_end->extruder_id = -1;
            }*/
            //add paths into my_loop => after that all ref are wrong!
            for (size_t i = travel_path_end.size() - 1; i < travel_path_end.size(); i--) {
                my_loop.paths.insert(my_loop.paths.begin() + nearest.idx_polyline_outter + child_paths_size + 1, travel_path_end[i]);
            }
            for (size_t i = travel_path_begin.size() - 1; i < travel_path_begin.size(); i--) {
                my_loop.paths.insert(my_loop.paths.begin() + nearest.idx_polyline_outter + 1, travel_path_begin[i]);
            }
        }
        //remove one-point extrusion
        //FIXME prevent this instead of patching here?
        for (size_t i = 0; i < my_loop.paths.size(); i++) {
            if (my_loop.paths[i].polyline.size() < 2) {
                if (my_loop.paths[i].polyline.size() == 1)
                    BOOST_LOG_TRIVIAL(warning) << "erase one-point extrusion : layer " << this->layer->id() << " " << my_loop.paths[i].polyline.points.front().x() << ":" << my_loop.paths[i].polyline.points.front().y() << "\n";
                my_loop.paths.erase(my_loop.paths.begin() + i);
                i--;
            }
        }

        //update for next loop
        childs.erase(childs.begin() + nearest.idx_children);
    }

    return my_loop;
}

bool PerimeterGeneratorLoop::is_internal_contour() const
{
    // An internal contour is a contour containing no other contours
    if (! this->is_contour)
                return false;
    for (const PerimeterGeneratorLoop &loop : this->children)
        if (loop.is_contour)
            return false;
    return true;
}

coord_t PerimeterGenerator::get_resolution(size_t perimeter_id, bool is_overhang, const Surface* srf) const
{
    coord_t reso = scale_t(this->print_config->resolution.value);
    if (reso == 0) reso = SCALED_EPSILON;
    return reso;
    //deactivated because with full perimeter on tube, the innermost perimeter can be very rough, and not a circle anymore.
    //if on top or bottom, use external resolution.
    //if (is_overhang || perimeter_id == 0)
    //    return reso;
    //if(srf && srf->has_pos_top())
    //    return reso;
    //// for each perimeter, reduce the precision by a factor 3
    //int mult = (int)std::pow(2, perimeter_id);
    //coord_t reso_internal = scale_t(this->print_config->resolution_internal.value);
    //if(reso_internal < reso * mult)
    //    return reso_internal;
    //return reso * mult;
}

}

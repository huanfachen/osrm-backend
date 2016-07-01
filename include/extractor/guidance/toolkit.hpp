#ifndef OSRM_GUIDANCE_TOOLKIT_HPP_
#define OSRM_GUIDANCE_TOOLKIT_HPP_

#include "util/bearing.hpp"
#include "util/coordinate.hpp"
#include "util/coordinate_calculation.hpp"
#include "util/guidance/toolkit.hpp"
#include "util/guidance/turn_lanes.hpp"
#include "util/typedefs.hpp"

#include "extractor/compressed_edge_container.hpp"
#include "extractor/query_node.hpp"
#include "extractor/suffix_table.hpp"

#include "extractor/guidance/classification_data.hpp"
#include "extractor/guidance/discrete_angle.hpp"
#include "extractor/guidance/intersection.hpp"
#include "extractor/guidance/turn_instruction.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/functional/hash.hpp>

namespace osrm
{
namespace extractor
{
namespace guidance
{

using util::guidance::LaneTupelIdPair;
using LaneDataIdMap = std::unordered_map<LaneTupelIdPair, LaneDataID, boost::hash<LaneTupelIdPair>>;

using util::guidance::angularDeviation;

namespace detail
{
const constexpr double DESIRED_SEGMENT_LENGTH = 10.0;

template <typename IteratorType>
util::Coordinate
getCoordinateFromCompressedRange(util::Coordinate current_coordinate,
                                 const IteratorType compressed_geometry_begin,
                                 const IteratorType compressed_geometry_end,
                                 const util::Coordinate final_coordinate,
                                 const std::vector<extractor::QueryNode> &query_nodes)
{
    const auto extractCoordinateFromNode =
        [](const extractor::QueryNode &node) -> util::Coordinate {
        return {node.lon, node.lat};
    };
    double distance_to_current_coordinate = 0;
    double distance_to_next_coordinate = 0;

    // get the length that is missing from the current segment to reach DESIRED_SEGMENT_LENGTH
    const auto getFactor = [](const double first_distance, const double second_distance) {
        BOOST_ASSERT(first_distance < detail::DESIRED_SEGMENT_LENGTH);
        double segment_length = second_distance - first_distance;
        BOOST_ASSERT(segment_length > 0);
        BOOST_ASSERT(second_distance >= detail::DESIRED_SEGMENT_LENGTH);
        double missing_distance = detail::DESIRED_SEGMENT_LENGTH - first_distance;
        return std::max(0., std::min(missing_distance / segment_length, 1.0));
    };

    for (auto compressed_geometry_itr = compressed_geometry_begin;
         compressed_geometry_itr != compressed_geometry_end;
         ++compressed_geometry_itr)
    {
        const auto next_coordinate =
            extractCoordinateFromNode(query_nodes[compressed_geometry_itr->node_id]);
        distance_to_next_coordinate =
            distance_to_current_coordinate +
            util::coordinate_calculation::haversineDistance(current_coordinate, next_coordinate);

        // reached point where coordinates switch between
        if (distance_to_next_coordinate >= detail::DESIRED_SEGMENT_LENGTH)
            return util::coordinate_calculation::interpolateLinear(
                getFactor(distance_to_current_coordinate, distance_to_next_coordinate),
                current_coordinate,
                next_coordinate);

        // prepare for next iteration
        current_coordinate = next_coordinate;
        distance_to_current_coordinate = distance_to_next_coordinate;
    }

    distance_to_next_coordinate =
        distance_to_current_coordinate +
        util::coordinate_calculation::haversineDistance(current_coordinate, final_coordinate);

    // reached point where coordinates switch between
    if (distance_to_current_coordinate < detail::DESIRED_SEGMENT_LENGTH &&
        distance_to_next_coordinate >= detail::DESIRED_SEGMENT_LENGTH)
        return util::coordinate_calculation::interpolateLinear(
            getFactor(distance_to_current_coordinate, distance_to_next_coordinate),
            current_coordinate,
            final_coordinate);
    else
        return final_coordinate;
}
} // namespace detail

// Finds a (potentially interpolated) coordinate that is DESIRED_SEGMENT_LENGTH away
// from the start of an edge
inline util::Coordinate
getRepresentativeCoordinate(const NodeID from_node,
                            const NodeID to_node,
                            const EdgeID via_edge_id,
                            const bool traverse_in_reverse,
                            const extractor::CompressedEdgeContainer &compressed_geometries,
                            const std::vector<extractor::QueryNode> &query_nodes)
{
    const auto extractCoordinateFromNode =
        [](const extractor::QueryNode &node) -> util::Coordinate {
        return {node.lon, node.lat};
    };

    // Uncompressed roads are simple, return the coordinate at the end
    if (!compressed_geometries.HasEntryForID(via_edge_id))
    {
        return extractCoordinateFromNode(traverse_in_reverse ? query_nodes[from_node]
                                                             : query_nodes[to_node]);
    }
    else
    {
        const auto &geometry = compressed_geometries.GetBucketReference(via_edge_id);

        const auto base_node_id = (traverse_in_reverse) ? to_node : from_node;
        const auto base_coordinate = extractCoordinateFromNode(query_nodes[base_node_id]);

        const auto final_node = (traverse_in_reverse) ? from_node : to_node;
        const auto final_coordinate = extractCoordinateFromNode(query_nodes[final_node]);

        if (traverse_in_reverse)
            return detail::getCoordinateFromCompressedRange(
                base_coordinate, geometry.rbegin(), geometry.rend(), final_coordinate, query_nodes);
        else
            return detail::getCoordinateFromCompressedRange(
                base_coordinate, geometry.begin(), geometry.end(), final_coordinate, query_nodes);
    }
}

inline bool isLowPriorityRoadClass(const FunctionalRoadClass road_class)
{
    return road_class == FunctionalRoadClass::LOW_PRIORITY_ROAD ||
           road_class == FunctionalRoadClass::SERVICE;
}

inline std::pair<std::string, std::string> getPrefixAndSuffix(const std::string &data)
{
    const auto suffix_pos = data.find_last_of(' ');
    if (suffix_pos == std::string::npos)
        return {};

    const auto prefix_pos = data.find_first_of(' ');
    auto result = std::make_pair(data.substr(0, prefix_pos), data.substr(suffix_pos + 1));
    boost::to_lower(result.first);
    boost::to_lower(result.second);
    return result;
}

inline bool requiresNameAnnounced(const std::string &from,
                                  const std::string &to,
                                  const SuffixTable &suffix_table)
{
    // first is empty and the second is not
    if (from.empty() && !to.empty())
        return true;

    // FIXME, handle in profile to begin with?
    // this uses the encoding of references in the profile, which is very BAD
    // Input for this function should be a struct separating streetname, suffix (e.g. road,
    // boulevard, North, West ...), and a list of references
    std::string from_name;
    std::string from_ref;
    std::string to_name;
    std::string to_ref;

    // Split from the format "{name} ({ref})" -> name, ref
    auto split = [](const std::string &name, std::string &out_name, std::string &out_ref) {
        const auto ref_begin = name.find_first_of('(');
        if (ref_begin != std::string::npos)
        {
            if (ref_begin != 0)
                out_name = name.substr(0, ref_begin - 1);
            const auto ref_end = name.find_first_of(')');
            out_ref = name.substr(ref_begin + 1, ref_end - ref_begin - 1);
        }
        else
        {
            out_name = name;
        }
    };

    split(from, from_name, from_ref);
    split(to, to_name, to_ref);

    // check similarity of names
    const auto names_are_empty = from_name.empty() && to_name.empty();
    const auto name_is_contained =
        boost::starts_with(from_name, to_name) || boost::starts_with(to_name, from_name);

    const auto checkForPrefixOrSuffixChange =
        [](const std::string &first, const std::string &second, const SuffixTable &suffix_table) {

            const auto first_prefix_and_suffixes = getPrefixAndSuffix(first);
            const auto second_prefix_and_suffixes = getPrefixAndSuffix(second);
            // reverse strings, get suffices and reverse them to get prefixes
            const auto checkTable = [&](const std::string str) {
                return str.empty() || suffix_table.isSuffix(str);
            };

            const bool is_prefix_change = [&]() -> bool {
                if (!checkTable(first_prefix_and_suffixes.first))
                    return false;
                if (!checkTable(first_prefix_and_suffixes.first))
                    return false;
                return !first.compare(first_prefix_and_suffixes.first.length(),
                                      std::string::npos,
                                      second,
                                      second_prefix_and_suffixes.first.length(),
                                      std::string::npos);
            }();

            const bool is_suffix_change = [&]() -> bool {
                if (!checkTable(first_prefix_and_suffixes.second))
                    return false;
                if (!checkTable(first_prefix_and_suffixes.second))
                    return false;
                return !first.compare(0,
                                      first.length() - first_prefix_and_suffixes.second.length(),
                                      second,
                                      0,
                                      second.length() - second_prefix_and_suffixes.second.length());
            }();

            return is_prefix_change || is_suffix_change;
        };

    const auto is_suffix_change = checkForPrefixOrSuffixChange(from_name, to_name, suffix_table);
    const auto names_are_equal = from_name == to_name || name_is_contained || is_suffix_change;
    const auto name_is_removed = !from_name.empty() && to_name.empty();
    // references are contained in one another
    const auto refs_are_empty = from_ref.empty() && to_ref.empty();
    const auto ref_is_contained =
        from_ref.empty() || to_ref.empty() ||
        (from_ref.find(to_ref) != std::string::npos || to_ref.find(from_ref) != std::string::npos);
    const auto ref_is_removed = !from_ref.empty() && to_ref.empty();

    const auto obvious_change =
        (names_are_empty && refs_are_empty) || (names_are_equal && ref_is_contained) ||
        (names_are_equal && refs_are_empty) || (ref_is_contained && name_is_removed) ||
        (names_are_equal && ref_is_removed) || is_suffix_change;

    return !obvious_change;
}

inline int getPriority(const FunctionalRoadClass road_class)
{
    // The road priorities indicate which roads can bee seen as more or less equal.
    // They are used in Fork-Discovery. Possibly should be moved to profiles post v5?
    // A fork can happen between road types that are at most 1 priority apart from each other
    const constexpr int road_priority[] = {
        10, 0, 10, 2, 10, 4, 10, 6, 10, 8, 10, 11, 10, 12, 10, 14};
    return road_priority[static_cast<int>(road_class)];
}

inline bool canBeSeenAsFork(const FunctionalRoadClass first, const FunctionalRoadClass second)
{
    // forks require similar road categories
    // Based on the priorities assigned above, we can set forks only if the road priorities match
    // closely.
    // Potentially we could include features like number of lanes here and others?
    // Should also be moved to profiles
    return std::abs(getPriority(first) - getPriority(second)) <= 1;
}

// To simplify handling of Left/Right hand turns, we can mirror turns and write an intersection
// handler only for one side. The mirror function turns a left-hand turn in a equivalent right-hand
// turn and vice versa.
inline ConnectedRoad mirror(ConnectedRoad road)
{
    const constexpr DirectionModifier::Enum mirrored_modifiers[] = {DirectionModifier::UTurn,
                                                                    DirectionModifier::SharpLeft,
                                                                    DirectionModifier::Left,
                                                                    DirectionModifier::SlightLeft,
                                                                    DirectionModifier::Straight,
                                                                    DirectionModifier::SlightRight,
                                                                    DirectionModifier::Right,
                                                                    DirectionModifier::SharpRight};

    if (angularDeviation(road.turn.angle, 0) > std::numeric_limits<double>::epsilon())
    {
        road.turn.angle = 360 - road.turn.angle;
        road.turn.instruction.direction_modifier =
            mirrored_modifiers[road.turn.instruction.direction_modifier];
    }
    return road;
}

inline bool hasRoundaboutType(const TurnInstruction instruction)
{
    using namespace extractor::guidance::TurnType;
    const constexpr TurnType::Enum valid_types[] = {TurnType::EnterRoundabout,
                                                    TurnType::EnterAndExitRoundabout,
                                                    TurnType::EnterRotary,
                                                    TurnType::EnterAndExitRotary,
                                                    TurnType::EnterRoundaboutIntersection,
                                                    TurnType::EnterAndExitRoundaboutIntersection,
                                                    TurnType::EnterRoundaboutAtExit,
                                                    TurnType::ExitRoundabout,
                                                    TurnType::EnterRotaryAtExit,
                                                    TurnType::ExitRotary,
                                                    TurnType::EnterRoundaboutIntersectionAtExit,
                                                    TurnType::ExitRoundaboutIntersection,
                                                    TurnType::StayOnRoundabout};
    const auto valid_end = valid_types + 13;
    return std::find(valid_types, valid_end, instruction.type) != valid_end;
}

// Public service vehicle lanes and similar can introduce additional lanes into the lane string that
// are not specifically marked for left/right turns. This function can be used from the profile to
// trim the lane string appropriately
//
// left|throught|
// in combination with lanes:psv:forward=1
// will be corrected to left|throught, since the final lane is not drivable.
// This is in contrast to a situation with lanes:psv:forward=0 (or not set) where left|through|
// represents left|through|through
inline std::string
trimLaneString(std::string lane_string, std::int32_t count_left, std::int32_t count_right)
{
    if (count_left)
    {
        bool sane = count_left < static_cast<std::int32_t>(lane_string.size());
        for (std::int32_t i = 0; i < count_left; ++i)
            // this is adjusted for our fake pipe. The moment cucumber can handle multiple escaped
            // pipes, the '&' part can be removed
            if (lane_string[i] != '|' && lane_string[i] != '&')
            {
                sane = false;
                break;
            }

        if (sane)
        {
            lane_string.erase(lane_string.begin(), lane_string.begin() + count_left);
        }
    }
    if (count_right)
    {
        bool sane = count_right < static_cast<std::int32_t>(lane_string.size());
        for (auto itr = lane_string.rbegin();
             itr != lane_string.rend() && itr != lane_string.rbegin() + count_right;
             ++itr)
        {
            if (*itr != '|' && *itr != '&')
            {
                sane = false;
                break;
            }
        }
        if (sane)
            lane_string.resize(lane_string.size() - count_right);
    }
    return lane_string;
}

inline bool entersRoundabout(const extractor::guidance::TurnInstruction instruction)
{
    return (instruction.type == extractor::guidance::TurnType::EnterRoundabout ||
            instruction.type == extractor::guidance::TurnType::EnterRotary ||
            instruction.type == extractor::guidance::TurnType::EnterRoundaboutIntersection ||
            instruction.type == extractor::guidance::TurnType::EnterRoundaboutAtExit ||
            instruction.type == extractor::guidance::TurnType::EnterRotaryAtExit ||
            instruction.type == extractor::guidance::TurnType::EnterRoundaboutIntersectionAtExit ||
            instruction.type == extractor::guidance::TurnType::EnterAndExitRoundabout ||
            instruction.type == extractor::guidance::TurnType::EnterAndExitRotary ||
            instruction.type == extractor::guidance::TurnType::EnterAndExitRotary);
}

inline bool leavesRoundabout(const extractor::guidance::TurnInstruction instruction)
{
    return (instruction.type == extractor::guidance::TurnType::ExitRoundabout ||
            instruction.type == extractor::guidance::TurnType::ExitRotary ||
            instruction.type == extractor::guidance::TurnType::ExitRoundaboutIntersection ||
            instruction.type == extractor::guidance::TurnType::EnterAndExitRoundabout ||
            instruction.type == extractor::guidance::TurnType::EnterAndExitRotary ||
            instruction.type == extractor::guidance::TurnType::EnterAndExitRoundaboutIntersection);
}

} // namespace guidance
} // namespace extractor
} // namespace osrm

#endif // OSRM_GUIDANCE_TOOLKIT_HPP_

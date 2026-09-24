# The first-party targets' link graph, for tools/check_link_cycles.py (M191).
#
# osc_write_link_graph(<file>) writes one "<target> <dependency>" line per
# link of an osc_* target, direct links only, aliases resolved. It runs at
# configure time, after every target is defined, so the check reads the graph
# of the configured build without re-running CMake on it.

function(_osc_collect_targets dir out)
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(sub IN LISTS subdirs)
        _osc_collect_targets("${sub}" sub_targets)
        list(APPEND targets ${sub_targets})
    endforeach()
    set(${out} ${targets} PARENT_SCOPE)
endfunction()

function(osc_write_link_graph file)
    _osc_collect_targets("${CMAKE_SOURCE_DIR}" targets)
    list(SORT targets)
    set(lines "")
    foreach(target IN LISTS targets)
        if(NOT target MATCHES "^osc_")
            continue()
        endif()
        # What it links (not for an interface library), and what it passes
        # on; a private link appears there as $<LINK_ONLY:...>.
        set(deps "")
        get_target_property(type ${target} TYPE)
        if(NOT type STREQUAL "INTERFACE_LIBRARY")
            get_target_property(linked ${target} LINK_LIBRARIES)
            if(linked)
                list(APPEND deps ${linked})
            endif()
        endif()
        get_target_property(passed ${target} INTERFACE_LINK_LIBRARIES)
        if(passed)
            list(APPEND deps ${passed})
        endif()
        set(seen "")
        foreach(dep IN LISTS deps)
            string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" dep "${dep}")
            # System libraries, flags and CMake's directory markers are not
            # targets.
            if(NOT TARGET "${dep}")
                continue()
            endif()
            get_target_property(real "${dep}" ALIASED_TARGET)
            if(real)
                set(dep "${real}")
            endif()
            if(NOT dep IN_LIST seen)
                list(APPEND seen "${dep}")
                string(APPEND lines "${target} ${dep}\n")
            endif()
        endforeach()
    endforeach()
    # Rewritten only when it changes.
    file(CONFIGURE OUTPUT "${file}" CONTENT "${lines}" @ONLY)
endfunction()

__doc__ = \
    """
    New recovery mechanism for elliptics that utilizes new iterators and metadata.
    NB! For now only "merge" mode is supported e.g. recovery within a group.

     * Find ranges that host stole from neighbours in routing table.
     * Start metadata-only iterator fo each range on local and remote hosts.
     * Sort iterators' outputs.
     * Computes diff between local and remote iterator.
     * Recover keys provided by diff using bulk APIs.
    """

import sys
import logging as log

from itertools import groupby

from ..range import IdRange, RecoveryRange
from ..route import RouteList
from ..iterator import Iterator
from ..time import Time
from ..utils.misc import format_id, elliptics_create_node, elliptics_create_session

# XXX: change me before BETA
sys.path.insert(0, "bindings/python/")
import elliptics

def get_ranges(ctx, routes, group_id):
    """
    For each record in RouteList create 1 or 2 RecoveryRange(s)
    Returns list of RecoveryRange`s
    """
    ranges = []
    for i, route in enumerate(routes):
        ekey, address = routes[i]
        prev_address = routes[i - 1].address
        next_ekey = routes[i + 1].key
        # For matching address but only in case where there is no wraparound
        if address == ctx.address and address != prev_address:
            log.debug("Processing route: {0}, {1}".format(format_id(ekey.id), address))
            start = ekey.id
            stop = next_ekey.id
            # If we wrapped around hash ring circle - split route into two distinct ranges
            if (stop < start):
                log.debug("Splitting range: {0}:{1}".format(
                    format_id(start), format_id(stop)))
                ranges.append(RecoveryRange(IdRange(IdRange.ID_MIN, stop), prev_address))
                ranges.append(RecoveryRange(IdRange(start, IdRange.ID_MAX), prev_address))
                created = (1, 2)
            else:
                ranges.append(RecoveryRange(IdRange(start, stop), prev_address))
                created = (1,)
            for i in created:
                log.debug("Created range: {0}, {1}".format(*ranges[-i]))
    return ranges


def run_iterator(ctx, group=None, address=None, routes=None, ranges=None, stats=None):
    """
    Runs iterator for all ranges on node specified by address
    TODO: We can group iterators by address and run them in parallel
    """
    node = elliptics_create_node(address=address, elog=ctx.elog)
    try:
        timestamp_range = ctx.timestamp.to_etime(), Time.time_max().to_etime()
        eid = routes.filter_by_address(address)[0].key
        key_ranges = [r.id_range for r in ranges]
        log.debug("Running iterator on node: {0}".format(address))
        results = Iterator(node, group).start(
            eid=eid,
            timestamp_range=timestamp_range,
            key_ranges=key_ranges,
            tmp_dir=ctx.tmp_dir,
            address=address,
        )
        for result in results:
            log.debug("Iterator {0} obtained: {1} record(s)".format(result.id_range, len(result)))
        stats.counter.records += len(results)
        stats.counter.iterations += 1
        return results
    except Exception as e:
        log.error("Iteration failed for: {0}: {1}".format(address, repr(e)))
        stats.counter.iterations -= 1
        return []

def sort(ctx, results, stats):
    """
    Runs sort routine for all iterator results
    """
    sorted_results = []
    for result in results:
        if len(result) == 0:
            log.debug("Sort skipped iterator results are empty")
            continue
        try:
            log.info("Processing sorting range: {0}".format(result.id_range))
            result.container.sort()
            sorted_results.append(result)
            stats.counter.remote += 1
        except Exception as e:
            log.error("Sort of {0} failed: {1}".format(result.id_range, e))
            stats.counter.sort -= 1
    return sorted_results

def diff(ctx, results, stats):
    """
    Compute differences between local and remote results.
    TODO: We can compute up to CPU_NUM diffs at max in parallel
    """
    diff_results = []
    for local, remote in results:
        try:
            if len(local) >= 0 and len(remote) == 0:
                log.info("Remote container is empty, skipping range: {0}".format(local.id_range))
                continue
            elif len(local) == 0 and len(remote) > 0:
                # If local container is empty and remote is not
                # then difference is whole remote container
                log.info("Local container is empty, recovering full range: {0}".format(local.id_range))
                result = remote
            else:
                log.info("Computing differences for: {0}".format(local.id_range))
                result = local.diff(remote)
            if len(result) > 0:
                diff_results.append(result)
            else:
                log.info("Resulting diff is empty, skipping range: {0}".format(local.id_range))
            stats.counter.diff += 1
        except Exception as e:
            log.error("Diff of {0} failed: {1}".format(local.id_range, e))
            stats.counter.diff -= 1
    return diff_results

def recover(ctx, diffs, group, stats):
    """
    Recovers difference between remote and local data.
    TODO: Group by diffs by address and process each group in parallel
    """
    result = True
    for diff in diffs:
        log.info("Recovering range: {0} for: {1}".format(diff.id_range, diff.address))

        # Here we cleverly splitting responses into ctx.batch_size batches
        for batch_id, batch in groupby(enumerate(diff),
                                        key=lambda x: x[0] / ctx.batch_size):
            keys = [elliptics.Id(r.key, group, 0) for _, r in batch]
            successes, failures = recover_keys(ctx, diff.address, group, keys)
            stats.counter.recover_key += successes
            stats.counter.recover_key -= failures
            result &= (failures == 0)
            log.debug("Recovered batch: {0}/{1} of size: {2}/{3}".format(
                batch_id * ctx.batch_size, len(diff), successes, failures))
    return result

def recover_keys(ctx, address, group, keys):
    """
    Bulk recovery of keys.
    """
    key_num = len(keys)

    log.debug("Reading {0} keys".format(key_num))
    try:
        log.debug("Creating node for: {0}".format(address))
        node = elliptics_create_node(address=address, elog=ctx.elog, flags=2)
        log.debug("Creating direct session: {0}".format(address))
        direct_session = elliptics_create_session(node=node,
                                                  group=group,
                                                  cflags=elliptics.command_flags.direct,
        )
        batch = direct_session.bulk_read(keys)
    except Exception as e:
        log.debug("Bulk read failed: {0} keys: {1}".format(key_num, e))
        return 0, key_num

    size = sum(len(v[1]) for v in batch)
    log.debug("Writing {0} keys: {1} bytes".format(key_num, size))
    try:
        log.debug("Creating node for: {0}".format(ctx.address))
        node = elliptics_create_node(address=ctx.address, elog=ctx.elog, flags=2)
        log.debug("Creating direct session: {0}".format(ctx.address))
        direct_session = elliptics_create_session(node=node,
                                                  group=group,
                                                  cflags=elliptics.command_flags.direct,
        )
        direct_session.bulk_write(batch)
    except Exception as e:
        log.debug("Bulk write failed: {0} keys: {1}".format(key_num, e))
        return 0, key_num
    return key_num, 0

def main(ctx):
    result = True
    ctx.stats.timer.main('started')

    # Run local iterators, sort them
    # For each host in route table run remote iterators in parallel
      # Iterate, sort, diff corresponding range, recover diff, return stats

    for group in ctx.groups:
        log.warning("Processing group: {0}".format(group))
        group_stats = ctx.stats['group_{0}'.format(group)]
        group_stats.timer.group('started')

        routes = RouteList(ctx.routes.filter_by_group_id(group))
        ranges = get_ranges(ctx, routes, group)
        log.debug("Recovery ranges: {0}".format(len(ranges)))
        if not ranges:
            log.warning("No ranges to recover in group: {0}".format(group))
            group_stats.timer.group('finished')
            continue
        assert all(address != ctx.address for _, address in ranges)

        log.warning("Running local iterators against: {0} range(s)".format(len(ranges)))
        group_stats.timer.group('iterator_local')
        local_results = run_iterator(
            ctx,
            group=group,
            address=ctx.address,
            routes=ctx.routes,
            ranges=ranges,
            stats=group_stats,
        )
        assert len(ranges) >= len(local_results)
        log.warning("Finished local iteration of: {0} range(s)".format(len(local_results)))

        log.warning("Sorting local iterator results")
        group_stats.timer.group('sort_local')
        sorted_local_results = sort(ctx, local_results, group_stats)
        assert len(local_results) >= len(sorted_local_results)
        log.warning("Sorted successfully: {0} local result(s)".format(len(sorted_local_results)))

        # For each address in computed recovery ranges run iterators
        # TODO: We should have `full_merge` mode that uses RouteList.addresses()
        addresses = set([r.address for r in ranges])
        for address in addresses:
            remote_stats = group_stats['remote_{0}'.format(address)]
            remote_stats.timer.remote('started')


            log.warning("Running remote iterators")
            remote_stats.timer.remote('iterator')
            # In merge mode we only using ranges that were stolen from `address`
            remote_ranges = [r for r in ranges if r.address == address]
            remote_results = run_iterator(
                ctx,
                group=group,
                address=address,
                routes=ctx.routes,
                ranges=remote_ranges,
                stats=remote_stats,
            )

            log.warning("Sorting remote iterator results")
            remote_stats.timer.remote('sort')
            sorted_remote_results = sort(ctx, remote_results, remote_stats)
            assert len(local_results) >= len(sorted_local_results)
            log.warning("Sorted successfully: {0} remote result(s)".format(len(sorted_local_results)))

            log.warning("Zipping remote results against local")
            zipped_results = []
            for remote_result in sorted_remote_results:
                try:
                    local_result, = [l for l in local_results if l.id_range == remote_result.id_range]
                    zipped_results.append((local_result, remote_result))
                except Exception as e:
                    log.error("Can't find local result corresponding to: {0}: {1}".format(remote_result.id_range, e))
            log.warning("Zipped {0} result(s)".format(len(zipped_results)))

            log.warning("Computing diff local vs remote")
            remote_stats.timer.remote('diff')
            diff_results = diff(ctx, zipped_results, remote_stats)
            assert len(sorted_remote_results) >= len(diff_results)
            log.warning("Computed differences: {0} diff(s)".format(len(diff_results)))

            log.warning("Recovering diffs")
            remote_stats.timer.remote('recover')
            result &= recover(ctx, diff_results, group, remote_stats)
            log.warning("Recovery finished, setting result to: {0}".format(result))
            remote_stats.timer.remote('finished')
        group_stats.timer.group('finished')
    ctx.stats.timer.main('finished')
    return result

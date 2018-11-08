from .operation_commands import TimeWatcher, process_operation_unsuccesful_finish_state
from .common import YtError, require
from .spec_builders import VanillaSpecBuilder
from .run_operation_commands import run_operation
from .cypress_commands import get, exists

def get_clickhouse_clique_spec_builder(instance_count,
                                       cypress_ytserver_clickhouse_path=None,
                                       host_ytserver_clickhouse_path=None,
                                       cypress_yson_config_path="//sys/clickhouse/config_files/config.yson",
                                       cypress_xml_config_path="//sys/clickhouse/config_files/config.xml",
                                       max_failed_job_count=None,
                                       spec=None):
    """Returns a spec builder for the clickhouse clique consisting of a given number of instances.

    :param instance_count: number of instances (also the number of jobs in the underlying vanilla operation).
    :type instance_count: int
    :param cypress_ytserver_clickhouse_path: path to the ytserver-clickhouse binary in Cypress or None.
    :type cypress_ytserver_clickhouse_path: str
    :param cypress_ytserver_clickhouse_path: path to the ytserver-clickhouse binary on the destination node or None.
    :type cypress_ytserver_clickhouse_path: str
    :param cypress_yson_config_path: path to the yson config for ytserver-clickhouse in Cypress.
    :type cypress_yson_config_path: str
    :param cypress_xml_config_path: path to the xml config for ytserver-clickhouse in Cypress.
    :type cypress_xml_config_path: str
    :param max_failed_job_count: maximum number of failed jobs that is allowed for the underlying vanilla operation.
    :type max_failed_job_count: int
    :param spec: other spec options.
    :type spec: dict

    .. seealso::  :ref:`operation_parameters`.
    """
    file_paths = [cypress_xml_config_path, cypress_yson_config_path]
    if cypress_ytserver_clickhouse_path is None and host_ytserver_clickhouse_path is None:
        cypress_ytserver_clickhouse_path = "//sys/clickhouse/bin/ytserver-clickhouse"
    require(cypress_ytserver_clickhouse_path is None or host_ytserver_clickhouse_path is None,
            lambda: YtError("Cypress ytserver-clickhouse binary path and host ytserver-clickhouse path "
                            "cannot be specified at the same time"))

    if cypress_ytserver_clickhouse_path is not None:
        executable_path = "./ytserver-clickhouse"
        file_paths.append(cypress_ytserver_clickhouse_path)
    else:
        executable_path = host_ytserver_clickhouse_path

    if spec is None:
        spec = dict()
    if "annotations" not in spec:
        spec["annotations"] = dict()
    if "expose_to_yql" not in spec["annotations"]:
        spec["annotations"]["expose_to_yql"] = True

    spec_builder = \
        VanillaSpecBuilder() \
            .begin_task("clickhouse_servers") \
                .job_count(instance_count) \
                .file_paths(file_paths) \
                .command('cat config.xml | sed -s "s/TCP_PORT/$YT_PORT_2/g" | sed -s "s/HTTP_PORT/$YT_PORT_3/g" | '
                         'sed -s "s/FQDN/$(hostname -f)/g" > config_patched.xml; '
                         '{} --config config.yson --xml-config config_patched.xml --instance-id $YT_JOB_ID '
                         '--clique-id $YT_OPERATION_ID --rpc-port $YT_PORT_0 --monitoring-port $YT_PORT_1 '
                         '--tcp-port $YT_PORT_2 --http-port $YT_PORT_3'
                         .format(executable_path)) \
                .memory_limit(10 * 2**30) \
                .port_count(4) \
            .end_task() \
            .max_failed_job_count(max_failed_job_count) \
            .spec(spec)

    return spec_builder

def start_clickhouse_clique(instance_count, client=None, **kwargs):
    """Starts a clickhouse clique consisting of a given number of instances.

    :param instance_count: number of instances (also the number of jobs in the underlying vanilla operation).
    :type instance_count: int

    .. seealso::  :ref:`operation_parameters`.
    """

    op = run_operation(get_clickhouse_clique_spec_builder(instance_count, **kwargs), client=client, sync=False)

    for state in op.get_state_monitor(TimeWatcher(1.0, 1.0, 0.0)):
        if state.is_running() and \
                exists("//sys/clickhouse/cliques/{0}".format(op.id), client=client) and \
                get("//sys/clickhouse/cliques/{0}/@count".format(op.id), client=client) == instance_count:
            return op
        elif state.is_unsuccessfully_finished():
            process_operation_unsuccesful_finish_state(op, state)
        else:
            op.printer(state)


#!/usr/bin/env python3

from yt_odin_checks.lib.check_runner import main


def run_check(logger, yt_client, options, states):
    return states.PARTIALLY_AVAILABLE_STATE, "Whoops"


if __name__ == "__main__":
    main(run_check)

"""ddfast: an exact double-dummy bridge solver.

The functions follow DDS 3.x's Python module ``dds3``: ``solve_board_pbn``,
``calc_all_tables_pbn`` and ``SolverContext`` take the same arguments and return the same
dicts. ``solve_boards_pbn`` is a batch form of ``solve_board_pbn`` (not in dds3). For code
written against dds3::

    import ddfast.dds3 as dds3

The endgame tables (about 1.2 GB) are loaded on first use from ``$DDFAST_TABLES`` (or
``build/tables`` under the current directory), and built there first if missing; call
``ddfast.init(path)`` to choose the directory.
"""
from ._ddfast import SolverContext, calc_all_tables_pbn, init, solve_board_pbn, solve_boards_pbn
from . import dds3

__all__ = ["SolverContext", "calc_all_tables_pbn", "dds3", "init", "solve_board_pbn", "solve_boards_pbn"]

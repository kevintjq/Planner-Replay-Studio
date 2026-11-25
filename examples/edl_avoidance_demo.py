"""Compatibility-style entry point rebuilt from the conversation requirements.

The original attachment was unavailable; this is not its recovered source.
"""
import sys
from slam_fpv.cli import main
if __name__=='__main__':
    sys.argv[1:]=['demo',*sys.argv[1:]]
    main()

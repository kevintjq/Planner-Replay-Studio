"""Rebuilt rear chase + oblique picture-in-picture synthetic demo."""
import sys
from slam_fpv.cli import main
if __name__=='__main__':
    sys.argv[1:]=['demo',*sys.argv[1:]]
    main()

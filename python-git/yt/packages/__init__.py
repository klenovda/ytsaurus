import os
import sys

class PackagesImporter(object):
    def __enter__(self):
        self.dir_name = os.path.dirname(__file__)
        sys.path.insert(1, self.dir_name)

    def __exit__(self, type, value, traceback):
        dir_index = -1
        for index, path in enumerate(sys.path):
            if path == self.dir_name:
                dir_index = index
        if dir_index != -1:
            sys.path.pop(dir_index)

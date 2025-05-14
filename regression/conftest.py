# conftest.py
import pytest
import os

def pytest_addoption(parser):
    parser.addoption(
        "--data-dir", action="store", default=None, help="Directory to search for data files", required=True
    )

    parser.addoption(
        "--data-dir", action="store", default=None, help="Directory to search for data files", required=True
    )

    parser.addoption(
        "--atol", action="store", default=1.0e-8, help="Absolute tolerance for floating point comparisons"
    )

    parser.addoption(
        "--rtol", action="store", default=1.0e-8, help="Relative tolerance for floating point comparisons"
    )


@pytest.fixture
def data_dir(request):
    """
    Fixture to get the data directory
    """
    return request.config.getoption("--data_dir")

@pytest.fixture
def atol(request):
    """
    Fixture to get the absolute tolerance
    """
    return request.config.getoption("--atol")

@pytest.fixture
def rtol(request):
    """
    Fixture to get the relative tolerance
    """
    return request.config.getoption("--rtol")



@pytest.fixture
def discovered_files(data_dir):
    """
    Walk through data directory and discover all SWMM input files
    """
    if data_dir is None:
        return []
    return [os.path.join(data_dir, f) for f in os.listdir(data_dir) if os.path.isfile(os.path.join(data_dir, f))]

def pytest_collection_modifyitems(items):
    for item in items:
        if item.originalname == 'test_compare_node_results' and 'input_file' in item.fixturenames:
            input_file = item.callspec.params['input_file']
            item._nodeid = f'{item.nodeid}_{os.path.basename(input_file)}'
import scipy


def main():
    f = scipy.fromfile(open("/home/will/Downloads/testdata.dat"), dtype=scipy.complex64)
    for value in f:
        print(value)
if __name__ == '__main__':
    main()